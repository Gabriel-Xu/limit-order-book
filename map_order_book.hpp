#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

// Baseline implementation built from standard containers.
//
// Each side is a std::map from price to a FIFO queue of orders, ordered so
// that begin() is always the best price. An id -> location index makes
// cancel/modify O(1) after the map lookup. Simple and obviously correct, which
// makes it the reference for differential testing, but every insert allocates
// and every lookup chases pointers.
class MapOrderBook {
public:
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
    if (new_price == loc.price && new_qty <= loc.order->qty) {
      loc.order->qty = new_qty; // reduce in place, keep priority
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

  std::size_t order_count() const { return index_.size(); }

private:
  struct Order {
    OrderId id;
    Quantity qty; // remaining
  };

  using Level = std::list<Order>; // FIFO; iterators stay valid on erase
  using BidLevels = std::map<Price, Level, std::greater<Price>>;
  using AskLevels = std::map<Price, Level, std::less<Price>>;

  struct Locator {
    Side side;
    Price price;
    Level::iterator order;
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
        Order &resting = queue.front();
        const Quantity fill = std::min(qty, resting.qty);
        out.push_back(Trade{aggressor, resting.id, level_price, fill});
        qty -= fill;
        resting.qty -= fill;
        if (resting.qty == 0) {
          index_.erase(resting.id);
          queue.pop_front();
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
    queue.push_back(Order{id, qty});
    index_.emplace(id, Locator{side, price, std::prev(queue.end())});
  }

  template <typename Levels>
  static void remove(Levels &levels, const Locator &loc) {
    auto level_it = levels.find(loc.price);
    level_it->second.erase(loc.order);
    if (level_it->second.empty())
      levels.erase(level_it);
  }

  template <typename Levels>
  static Quantity level_volume(const Levels &levels, Price price) {
    auto it = levels.find(price);
    if (it == levels.end())
      return 0;
    Quantity total = 0;
    for (const Order &o : it->second)
      total += o.qty;
    return total;
  }

  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, Locator> index_;
};

static_assert(OrderBook<MapOrderBook>);
