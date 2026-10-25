#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "order_book.hpp"
#include "types.hpp"

// Improved implementation of the baseline that pre-allocates a pool of orders
// to avoid heap allocations on every insert. This also improves cache locality
// by storing the orders in a contiguous block of memory. Freed slots are kept
// on an intrusive free list threaded through the same `next` link that chains
// orders within a level (a slot is never in both). Each level is an intrusive
// doubly-linked list whose links are indices into the pool instead of
// pointers. This reduces the memory overhead, pointer chasing, and
// fragmentation, and because links are indices the pool can grow without
// invalidating them.
class PoolOrderBook {
public:
  explicit PoolOrderBook(std::size_t capacity = 1024) {
    pool_.reserve(capacity);
    index_.reserve(capacity);
  }

  bool add_limit(OrderId id, Side side, Price price, Quantity qty,
                 std::vector<Trade> &out) {
    if (qty == 0 || index_.contains(id))
      return false;

    if (side == Side::Buy) {
      match(asks_, id, side, price, qty, out);
      if (qty > 0)
        rest(bids_, id, side, price, qty);
    } else {
      match(bids_, id, side, price, qty, out);
      if (qty > 0)
        rest(asks_, id, side, price, qty);
    }
    return true;
  }

  void add_market(OrderId id, Side side, Quantity qty,
                  std::vector<Trade> &out) {
    if (side == Side::Buy)
      match(asks_, id, side, std::nullopt, qty, out);
    else
      match(bids_, id, side, std::nullopt, qty, out);
  }

  bool cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end())
      return false;

    const Locator &loc = it->second;
    if (loc.side == Side::Buy)
      remove(bids_, loc);
    else
      remove(asks_, loc);
    index_.erase(it);
    return true;
  }

  bool modify(OrderId id, Price new_price, Quantity new_qty,
              std::vector<Trade> &out) {
    auto it = index_.find(id);
    if (it == index_.end())
      return false;
    if (new_qty == 0)
      return cancel(id);

    const Locator &loc = it->second;
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
    if (bids_.empty())
      return std::nullopt;
    return bids_.begin()->first;
  }

  std::optional<Price> best_ask() const {
    if (asks_.empty())
      return std::nullopt;
    return asks_.begin()->first;
  }

  // Total resting quantity at one price. O(orders at level); meant for
  // tests and inspection, not the hot path.
  Quantity volume_at(Side side, Price price) const {
    return side == Side::Buy ? level_volume(bids_, price)
                             : level_volume(asks_, price);
  }

  std::size_t order_count() const {
    return index_.size();
  }

private:
  using PoolIndex = std::uint32_t;
  static constexpr PoolIndex kNull = UINT32_MAX;

  struct Order {
    OrderId id;
    Quantity qty;  // remaining
    PoolIndex next; // next in level while resting, next free slot otherwise
    PoolIndex prev;
  };

  struct Level {
    PoolIndex head = kNull;
    PoolIndex tail = kNull;

    bool empty() const {
      return head == kNull;
    }
  };

  using BidLevels = std::map<Price, Level, std::greater<Price>>;
  using AskLevels = std::map<Price, Level, std::less<Price>>;

  struct Locator {
    Side side;
    Price price;
    PoolIndex order;
  };

  // Fill `qty` against the opposite side, best price first, FIFO within a
  // level. `limit` of nullopt means a market order (no price constraint).
  template <typename Levels>
  void match(Levels &opposite, OrderId aggressor, Side side,
             std::optional<Price> limit, Quantity &qty,
             std::vector<Trade> &out) {
    while (qty > 0 && !opposite.empty()) {
      auto level_it = opposite.begin();
      const Price level_price = level_it->first;
      if (limit && !crosses(side, level_price, *limit))
        break;

      Level &queue = level_it->second;
      while (qty > 0 && !queue.empty()) {
        const PoolIndex head = queue.head;
        Order &resting = pool_[head];
        const Quantity fill = std::min(qty, resting.qty);
        out.push_back(Trade{aggressor, resting.id, level_price, fill});
        qty -= fill;
        resting.qty -= fill;
        if (resting.qty == 0) {
          index_.erase(resting.id);
          unlink(queue, head);
          release(head);
        }
      }
      if (queue.empty())
        opposite.erase(level_it);
    }
  }

  static bool crosses(Side aggressor_side, Price resting_price, Price limit) {
    return aggressor_side == Side::Buy ? resting_price <= limit
                                       : resting_price >= limit;
  }

  template <typename Levels>
  void rest(Levels &levels, OrderId id, Side side, Price price, Quantity qty) {
    Level &queue = levels[price];
    const PoolIndex i = allocate(id, qty);
    push_back(queue, i);
    index_.emplace(id, Locator{side, price, i});
  }

  template <typename Levels>
  void remove(Levels &levels, const Locator &loc) {
    auto level_it = levels.find(loc.price);
    unlink(level_it->second, loc.order);
    release(loc.order);
    if (level_it->second.empty())
      levels.erase(level_it);
  }

  // Take a slot from the free list, growing the pool if it is exhausted.
  // Growth may reallocate `pool_`, so no Order& may be held across this call.
  PoolIndex allocate(OrderId id, Quantity qty) {
    PoolIndex i = free_head_;
    if (i == kNull) {
      i = static_cast<PoolIndex>(pool_.size());
      pool_.emplace_back();
    } else {
      free_head_ = pool_[i].next;
    }
    pool_[i] = Order{id, qty, kNull, kNull};
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
    if (order.prev != kNull)
      pool_[order.prev].next = order.next;
    else
      queue.head = order.next;
    if (order.next != kNull)
      pool_[order.next].prev = order.prev;
    else
      queue.tail = order.prev;
  }

  template <typename Levels>
  Quantity level_volume(const Levels &levels, Price price) const {
    auto it = levels.find(price);
    if (it == levels.end())
      return 0;
    Quantity total = 0;
    for (PoolIndex i = it->second.head; i != kNull; i = pool_[i].next)
      total += pool_[i].qty;
    return total;
  }

  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, Locator> index_;
  std::vector<Order> pool_;
  PoolIndex free_head_ = kNull;
};

static_assert(OrderBook<PoolOrderBook>);
