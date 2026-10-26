#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Improves upon PoolOrderBook by using std::vector instead of std::map and
// std::unordered_map for the levels and index, respectively. This improves
// cache locality and removes unnecessary lookups. However, this operates under
// the assumption that the price range is limited to a known range and that the
// order IDs are sequential integers.
//
// Each side is a ladder: one Level per tick in [min_price, max_price], plus
// the best price and a count of non-empty levels. When the best level empties
// we scan toward worse prices for the next non-empty one, which is short in a
// dense book; the count lets an emptied side skip the scan entirely. Limit
// orders (and modifies) priced outside the range are rejected. The id index
// grows on demand, so ids need only be small, not bounded up front.
class FlatOrderBook {
public:
  explicit FlatOrderBook(Price min_price = 0, Price max_price = 1'000'000,
                         std::size_t order_count = 1'000'000,
                         std::size_t capacity = 4096)
      : min_price_(min_price), max_price_(max_price),
        bids_{std::vector<Level>(level_count()), min_price - 1},
        asks_{std::vector<Level>(level_count()), max_price + 1} {
    pool_.reserve(capacity);
    index_.assign(order_count, Locator{Side::Buy, 0, k_null});
  }

  bool add_limit(OrderId id, Side side, Price price, Quantity qty,
                 std::vector<Trade> &out) {
    if (qty == 0 || resting(id) || !in_range(price))
      return false;

    if (side == Side::Buy) {
      match<Side::Sell>(id, price, qty, out);
      if (qty > 0)
        rest<Side::Buy>(id, price, qty);
    } else {
      match<Side::Buy>(id, price, qty, out);
      if (qty > 0)
        rest<Side::Sell>(id, price, qty);
    }
    return true;
  }

  void add_market(OrderId id, Side side, Quantity qty,
                  std::vector<Trade> &out) {
    if (side == Side::Buy)
      match<Side::Sell>(id, std::nullopt, qty, out);
    else
      match<Side::Buy>(id, std::nullopt, qty, out);
  }

  bool cancel(OrderId id) {
    if (!resting(id))
      return false;

    Locator &loc = index_[id];
    if (loc.side == Side::Buy)
      remove<Side::Buy>(loc);
    else
      remove<Side::Sell>(loc);
    loc.order = k_null;
    --resting_count_;
    return true;
  }

  bool modify(OrderId id, Price new_price, Quantity new_qty,
              std::vector<Trade> &out) {
    if (!resting(id))
      return false;
    if (new_qty == 0)
      return cancel(id);
    // Reject before cancelling, so a bad price leaves the order untouched.
    if (!in_range(new_price))
      return false;

    const Locator &loc = index_[id];
    Order &order = pool_[loc.order];
    if (new_price == loc.price && new_qty <= order.qty) {
      order.qty = new_qty; // reduce in place, keep priority
      return true;
    }

    const Side side = loc.side;
    cancel(id);
    add_limit(id, side, new_price, new_qty, out);
    return true;
  }

  std::optional<Price> best_bid() const {
    if (bids_.active == 0)
      return std::nullopt;
    return bids_.best;
  }

  std::optional<Price> best_ask() const {
    if (asks_.active == 0)
      return std::nullopt;
    return asks_.best;
  }

  // Total resting quantity at one price. O(orders at level); meant for
  // tests and inspection, not the hot path.
  Quantity volume_at(Side side, Price price) const {
    if (!in_range(price))
      return 0;
    const Ladder &ladder = side == Side::Buy ? bids_ : asks_;
    Quantity total = 0;
    for (PoolIndex i = ladder.levels[offset(price)].head; i != k_null;
         i = pool_[i].next)
      total += pool_[i].qty;
    return total;
  }

  std::size_t order_count() const { return resting_count_; }

private:
  using PoolIndex = std::uint32_t;
  static constexpr PoolIndex k_null = UINT32_MAX;

  struct Order {
    OrderId id;
    Quantity qty;   // remaining
    PoolIndex next; // next in level while resting, next free slot otherwise
    PoolIndex prev;
  };

  struct Level {
    PoolIndex head = k_null;
    PoolIndex tail = k_null;

    bool empty() const { return head == k_null; }
  };

  // One side of the book. `best` is only meaningful while `active > 0`; when
  // the side is empty it holds a sentinel one tick outside the range, worse
  // than every real price, so `better(price, best)` needs no special case.
  struct Ladder {
    std::vector<Level> levels;
    Price best;
    std::size_t active = 0; // non-empty levels
  };

  struct Locator {
    Side side;
    Price price;
    PoolIndex order;
  };

  std::size_t level_count() const {
    return static_cast<std::size_t>(max_price_ - min_price_ + 1);
  }

  bool in_range(Price price) const {
    return price >= min_price_ && price <= max_price_;
  }

  std::size_t offset(Price price) const {
    return static_cast<std::size_t>(price - min_price_);
  }

  bool resting(OrderId id) const {
    return id < index_.size() && index_[id].order != k_null;
  }

  template <Side S>
  Ladder &ladder() {
    if constexpr (S == Side::Buy)
      return bids_;
    else
      return asks_;
  }

  // True if `a` is a better resting price than `b` on side S.
  template <Side S>
  static bool better(Price a, Price b) {
    if constexpr (S == Side::Buy)
      return a > b;
    else
      return a < b;
  }

  // Fill `qty` against resting side S, best price first, FIFO within a
  // level. `limit` of nullopt means a market order (no price constraint).
  template <Side S>
  void match(OrderId aggressor, std::optional<Price> limit, Quantity &qty,
             std::vector<Trade> &out) {
    Ladder &book = ladder<S>();
    while (qty > 0 && book.active > 0) {
      const Price level_price = book.best;
      if (limit && better<S>(*limit, level_price))
        break;

      Level &queue = book.levels[offset(level_price)];
      while (qty > 0 && !queue.empty()) {
        const PoolIndex head = queue.head;
        Order &resting = pool_[head];
        const Quantity fill = std::min(qty, resting.qty);
        out.push_back(Trade{aggressor, resting.id, level_price, fill});
        qty -= fill;
        resting.qty -= fill;
        if (resting.qty == 0) {
          index_[resting.id].order = k_null;
          --resting_count_;
          unlink(queue, head);
          release(head);
        }
      }
      if (queue.empty())
        level_emptied<S>(level_price);
    }
  }

  template <Side S>
  void rest(OrderId id, Price price, Quantity qty) {
    Ladder &book = ladder<S>();
    Level &queue = book.levels[offset(price)];
    if (queue.empty()) {
      ++book.active;
      if (better<S>(price, book.best))
        book.best = price;
    }
    const PoolIndex i = allocate(id, qty);
    push_back(queue, i);

    if (id >= index_.size())
      index_.resize(std::max<std::size_t>(id + 1, 2 * index_.size()),
                    Locator{Side::Buy, 0, k_null});
    index_[id] = Locator{S, price, i};
    ++resting_count_;
  }

  template <Side S>
  void remove(const Locator &loc) {
    Level &queue = ladder<S>().levels[offset(loc.price)];
    unlink(queue, loc.order);
    release(loc.order);
    if (queue.empty())
      level_emptied<S>(loc.price);
  }

  // Bookkeeping after the level at `price` on side S becomes empty: if it was
  // the best, walk toward worse prices to the next non-empty level.
  template <Side S>
  void level_emptied(Price price) {
    Ladder &book = ladder<S>();
    if (--book.active == 0) {
      book.best = S == Side::Buy ? min_price_ - 1 : max_price_ + 1;
      return;
    }
    if (price != book.best)
      return;
    // Another level is non-empty, so this terminates inside the range.
    do {
      book.best += S == Side::Buy ? -1 : 1;
    } while (book.levels[offset(book.best)].empty());
  }

  // Take a slot from the free list, growing the pool if it is exhausted.
  // Growth may reallocate `pool_`, so no Order& may be held across this call.
  PoolIndex allocate(OrderId id, Quantity qty) {
    PoolIndex i = free_head_;
    if (i == k_null) {
      i = static_cast<PoolIndex>(pool_.size());
      pool_.emplace_back();
    } else {
      free_head_ = pool_[i].next;
    }
    pool_[i] = Order{id, qty, k_null, k_null};
    return i;
  }

  void release(PoolIndex i) {
    pool_[i].next = free_head_;
    free_head_ = i;
  }

  void push_back(Level &queue, PoolIndex i) {
    pool_[i].prev = queue.tail;
    if (queue.empty())
      queue.head = i;
    else
      pool_[queue.tail].next = i;
    queue.tail = i;
  }

  void unlink(Level &queue, PoolIndex i) {
    const Order &order = pool_[i];
    if (order.prev != k_null)
      pool_[order.prev].next = order.next;
    else
      queue.head = order.next;
    if (order.next != k_null)
      pool_[order.next].prev = order.prev;
    else
      queue.tail = order.prev;
  }

  Price min_price_;
  Price max_price_;
  Ladder bids_;
  Ladder asks_;
  std::vector<Locator> index_;
  std::vector<Order> pool_;
  PoolIndex free_head_ = k_null;
  std::size_t resting_count_ = 0;
};

static_assert(OrderBook<FlatOrderBook>);
