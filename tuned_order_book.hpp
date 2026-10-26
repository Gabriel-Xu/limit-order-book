#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace tuned_detail {

inline constexpr std::size_t k_cache_line = 64;
inline constexpr std::size_t k_huge_page = 2 * 1024 * 1024;

constexpr std::size_t round_up(std::size_t n, std::size_t align) {
  return (n + align - 1) / align * align;
}

// Allocator for the flat arrays. Allocations of at least one huge page are
// 2 MiB aligned and marked MADV_HUGEPAGE, so the price ladders and id index
// are covered by a handful of TLB entries instead of thousands of 4 KiB ones.
// Smaller allocations are cache-line aligned, so no element straddles a line.
template <typename T>
struct HugePageAllocator {
  using value_type = T;

  HugePageAllocator() = default;
  template <typename U>
  HugePageAllocator(const HugePageAllocator<U> &) {}

  T *allocate(std::size_t n) {
    const std::size_t bytes = std::max<std::size_t>(n * sizeof(T), 1);
    const std::size_t align = bytes >= k_huge_page ? k_huge_page : k_cache_line;
    const std::size_t size = round_up(bytes, align);
    void *p = std::aligned_alloc(align, size);
    if (p == nullptr)
      throw std::bad_alloc();
#if defined(__linux__) && defined(MADV_HUGEPAGE)
    if (align == k_huge_page)
      madvise(p, size, MADV_HUGEPAGE); // advisory; failure just means 4 KiB
#endif
    return static_cast<T *>(p);
  }

  void deallocate(T *p, std::size_t) { std::free(p); }

  friend bool operator==(const HugePageAllocator &, const HugePageAllocator &) {
    return true;
  }
};

template <typename T>
using FlatVector = std::vector<T, HugePageAllocator<T>>;

// Two-level bitmap over price levels: bit i of `words_` is set when level i is
// non-empty, and bit w of `summary_` is set when words_[w] != 0. Finding the
// next non-empty level is a masked lookup in one word, and on a miss a scan
// of the summary, which is 64x shorter than the words (245 words for the
// default million-tick range) and almost always hits in its first word.
class LevelBitmap {
public:
  explicit LevelBitmap(std::size_t bits)
      : words_((bits + 63) / 64), summary_((words_.size() + 63) / 64) {}

  void set(std::size_t i) {
    const std::size_t w = i / 64;
    if (words_[w] == 0)
      summary_[w / 64] |= bit(w);
    words_[w] |= bit(i);
  }

  void clear(std::size_t i) {
    const std::size_t w = i / 64;
    words_[w] &= ~bit(i);
    if (words_[w] == 0)
      summary_[w / 64] &= ~bit(w);
  }

  // Highest set bit at or below `i`. One must exist.
  std::size_t find_at_or_below(std::size_t i) const {
    std::size_t w = i / 64;
    std::uint64_t m = words_[w] & at_or_below(i);
    if (m == 0) {
      w = highest_word_below(w);
      m = words_[w];
    }
    return w * 64 + highest(m);
  }

  // Lowest set bit at or above `i`. One must exist.
  std::size_t find_at_or_above(std::size_t i) const {
    std::size_t w = i / 64;
    std::uint64_t m = words_[w] & at_or_above(i);
    if (m == 0) {
      w = lowest_word_above(w);
      m = words_[w];
    }
    return w * 64 + lowest(m);
  }

private:
  static std::uint64_t bit(std::size_t i) {
    return std::uint64_t{1} << (i % 64);
  }
  static std::uint64_t at_or_below(std::size_t i) {
    return ~std::uint64_t{0} >> (63 - i % 64);
  }
  static std::uint64_t at_or_above(std::size_t i) {
    return ~std::uint64_t{0} << (i % 64);
  }
  static std::size_t highest(std::uint64_t m) {
    return 63 - static_cast<std::size_t>(std::countl_zero(m));
  }
  static std::size_t lowest(std::uint64_t m) {
    return static_cast<std::size_t>(std::countr_zero(m));
  }

  // A set bit below word `w` exists, so `w > 0` and the scan terminates.
  std::size_t highest_word_below(std::size_t w) const {
    std::size_t s = (w - 1) / 64;
    std::uint64_t m = summary_[s] & at_or_below(w - 1);
    while (m == 0)
      m = summary_[--s];
    return s * 64 + highest(m);
  }

  // A set bit above word `w` exists, so `w + 1` is a valid word.
  std::size_t lowest_word_above(std::size_t w) const {
    std::size_t s = (w + 1) / 64;
    std::uint64_t m = summary_[s] & at_or_above(w + 1);
    while (m == 0)
      m = summary_[++s];
    return s * 64 + lowest(m);
  }

  FlatVector<std::uint64_t> words_;
  FlatVector<std::uint64_t> summary_;
};

} // namespace tuned_detail

// Builds on FlatOrderBook with the same price ladder and id-indexed layout,
// tuned for the memory system:
//
// - A two-level bitmap of non-empty levels per side, so finding the next best
//   price after a level empties is a couple of bit scans instead of a walk
//   over empty levels. This bounds the worst case in a sparse book.
// - Order is 16 bytes (four per cache line) by storing the id as 32 bits,
//   which the id-indexed design already requires to be small.
// - The id index entry is 8 bytes (pool slot plus price offset, with the side
//   packed in the top bit), a third of Flat's, so cancels of scattered ids
//   touch fewer cache lines and pages.
// - The ladders and index live on 2 MiB huge pages where the kernel allows,
//   cutting dTLB misses on random-id cancels and price drift.
//
// There is no hot/cold split of Order: every field is read on the paths that
// touch it (match reads id, qty and next; cancel reads next and prev), so
// splitting would add a second cache line per order rather than save one. The
// split that matters is already structural: the id index is only touched on
// cancel/modify and when an order fills, never while walking a level.
//
// Limit order ids must be below 2^32 and prices within [min_price,
// max_price]; anything else is rejected, as in FlatOrderBook.
class TunedOrderBook {
public:
  explicit TunedOrderBook(Price min_price = 0, Price max_price = 1'000'000,
                          std::size_t order_count = 1'000'000,
                          std::size_t capacity = 4096)
      : min_price_(min_price), max_price_(max_price),
        bids_{Levels(level_count()), Bitmap(level_count()), min_price - 1},
        asks_{Levels(level_count()), Bitmap(level_count()), max_price + 1} {
    assert(max_price >= min_price && level_count() <= k_sell_bit);
    pool_.reserve(capacity);
    index_.resize(order_count);
  }

  bool add_limit(OrderId id, Side side, Price price, Quantity qty,
                 std::vector<Trade> &out) {
    if (qty == 0 || id > k_max_id || resting(id) || !in_range(price))
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
    if (side_of(loc) == Side::Buy)
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
    if (new_price == price_of(loc) && new_qty <= order.qty) {
      order.qty = new_qty; // reduce in place, keep priority
      return true;
    }

    const Side side = side_of(loc);
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
  static constexpr OrderId k_max_id = UINT32_MAX;
  static constexpr std::uint32_t k_sell_bit = std::uint32_t{1} << 31;

  struct Order {
    std::uint32_t id; // fits: limit order ids are checked against k_max_id
    Quantity qty;     // remaining
    PoolIndex next;   // next in level while resting, next free slot otherwise
    PoolIndex prev;
  };
  static_assert(sizeof(Order) == 16);

  struct Level {
    PoolIndex head = k_null;
    PoolIndex tail = k_null;

    bool empty() const { return head == k_null; }
  };
  static_assert(sizeof(Level) == 8);

  // Where a resting order lives. `level` is the price offset from min_price,
  // with k_sell_bit set for asks; `order` is k_null when the id isn't resting.
  struct Locator {
    PoolIndex order = k_null;
    std::uint32_t level = 0;
  };
  static_assert(sizeof(Locator) == 8);

  using Levels = tuned_detail::FlatVector<Level>;
  using Bitmap = tuned_detail::LevelBitmap;

  // One side of the book. `best` is only meaningful while `active > 0`; when
  // the side is empty it holds a sentinel one tick outside the range, worse
  // than every real price, so `better(price, best)` needs no special case.
  struct Ladder {
    Levels levels;
    Bitmap occupied;
    Price best;
    std::size_t active = 0; // non-empty levels
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

  Price price_at(std::size_t level) const {
    return min_price_ + static_cast<Price>(level);
  }

  static Side side_of(const Locator &loc) {
    return (loc.level & k_sell_bit) != 0 ? Side::Sell : Side::Buy;
  }

  Price price_of(const Locator &loc) const {
    return price_at(loc.level & ~k_sell_bit);
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

      const std::size_t level = offset(level_price);
      Level &queue = book.levels[level];
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
        level_emptied<S>(level);
    }
  }

  template <Side S>
  void rest(OrderId id, Price price, Quantity qty) {
    Ladder &book = ladder<S>();
    const std::size_t level = offset(price);
    Level &queue = book.levels[level];
    if (queue.empty()) {
      ++book.active;
      book.occupied.set(level);
      if (better<S>(price, book.best))
        book.best = price;
    }
    const PoolIndex i = allocate(id, qty);
    push_back(queue, i);

    if (id >= index_.size())
      index_.resize(std::max<std::size_t>(id + 1, 2 * index_.size()));
    const std::uint32_t side_bit = S == Side::Sell ? k_sell_bit : 0;
    index_[id] = Locator{i, static_cast<std::uint32_t>(level) | side_bit};
    ++resting_count_;
  }

  template <Side S>
  void remove(const Locator &loc) {
    const std::size_t level = loc.level & ~k_sell_bit;
    Level &queue = ladder<S>().levels[level];
    unlink(queue, loc.order);
    release(loc.order);
    if (queue.empty())
      level_emptied<S>(level);
  }

  // Bookkeeping after `level` on side S becomes empty: if it was the best,
  // jump to the next non-empty level toward worse prices via the bitmap.
  template <Side S>
  void level_emptied(std::size_t level) {
    Ladder &book = ladder<S>();
    book.occupied.clear(level);
    if (--book.active == 0) {
      book.best = S == Side::Buy ? min_price_ - 1 : max_price_ + 1;
      return;
    }
    if (level != offset(book.best))
      return;
    // Another level is non-empty, so the search finds one inside the range.
    if constexpr (S == Side::Buy)
      book.best = price_at(book.occupied.find_at_or_below(level));
    else
      book.best = price_at(book.occupied.find_at_or_above(level));
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
    pool_[i] = Order{static_cast<std::uint32_t>(id), qty, k_null, k_null};
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
  tuned_detail::FlatVector<Locator> index_;
  tuned_detail::FlatVector<Order> pool_;
  PoolIndex free_head_ = k_null;
  std::size_t resting_count_ = 0;
};

static_assert(OrderBook<TunedOrderBook>);
