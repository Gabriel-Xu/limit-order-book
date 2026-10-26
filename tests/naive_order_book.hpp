#pragma once

#include "order_book.hpp"
#include "types.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

// Deliberately naive reference book used as a test oracle. Every order lives
// in one flat vector and every operation is a linear scan, so it is far too
// slow to benchmark but simple enough to check by eye. Faster books are
// differentially tested against it.
class NaiveOrderBook {
public:
  bool add_limit(OrderId id, Side side, Price price, Quantity qty,
                 std::vector<Trade> &out) {
    if (qty == 0 || find(id) != orders_.end())
      return false;

    match(id, side, price, qty, out);
    if (qty > 0)
      orders_.push_back(Order{id, side, price, qty, next_seq_++});
    return true;
  }

  void add_market(OrderId id, Side side, Quantity qty,
                  std::vector<Trade> &out) {
    match(id, side, std::nullopt, qty, out);
  }

  bool cancel(OrderId id) {
    auto it = find(id);
    if (it == orders_.end())
      return false;
    orders_.erase(it);
    return true;
  }

  bool modify(OrderId id, Price new_price, Quantity new_qty,
              std::vector<Trade> &out) {
    auto it = find(id);
    if (it == orders_.end())
      return false;
    if (new_qty == 0)
      return cancel(id);

    if (new_price == it->price && new_qty <= it->qty) {
      it->qty = new_qty; // reduce in place, keep priority
      return true;
    }

    const Side side = it->side;
    orders_.erase(it);
    add_limit(id, side, new_price, new_qty, out);
    return true;
  }

  std::optional<Price> best_bid() const {
    std::optional<Price> best;
    for (const Order &o : orders_)
      if (o.side == Side::Buy && (!best || o.price > *best))
        best = o.price;
    return best;
  }

  std::optional<Price> best_ask() const {
    std::optional<Price> best;
    for (const Order &o : orders_)
      if (o.side == Side::Sell && (!best || o.price < *best))
        best = o.price;
    return best;
  }

  Quantity volume_at(Side side, Price price) const {
    Quantity total = 0;
    for (const Order &o : orders_)
      if (o.side == side && o.price == price)
        total += o.qty;
    return total;
  }

  std::size_t order_count() const { return orders_.size(); }

private:
  struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity qty;      // remaining
    std::uint64_t seq; // arrival order, for time priority
  };

  std::vector<Order>::iterator find(OrderId id) {
    return std::find_if(orders_.begin(), orders_.end(),
                        [id](const Order &o) { return o.id == id; });
  }

  // Repeatedly trade with the single best eligible resting order: best price,
  // then earliest arrival. `limit` of nullopt means a market order.
  void match(OrderId aggressor, Side side, std::optional<Price> limit,
             Quantity &qty, std::vector<Trade> &out) {
    while (qty > 0) {
      auto best = orders_.end();
      for (auto it = orders_.begin(); it != orders_.end(); ++it) {
        if (it->side == side)
          continue;
        if (limit &&
            (side == Side::Buy ? it->price > *limit : it->price < *limit))
          continue;
        if (best == orders_.end() || better(side, *it, *best))
          best = it;
      }
      if (best == orders_.end())
        break;

      const Quantity fill = std::min(qty, best->qty);
      out.push_back(Trade{aggressor, best->id, best->price, fill});
      qty -= fill;
      best->qty -= fill;
      if (best->qty == 0)
        orders_.erase(best);
    }
  }

  static bool better(Side aggressor_side, const Order &a, const Order &b) {
    if (a.price != b.price)
      return aggressor_side == Side::Buy ? a.price < b.price
                                         : a.price > b.price;
    return a.seq < b.seq;
  }

  std::vector<Order> orders_;
  std::uint64_t next_seq_ = 0;
};

static_assert(OrderBook<NaiveOrderBook>);
