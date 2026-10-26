#pragma once

#include "map_order_book.hpp"
#include "order_book.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

// A pre-generated stream of order book events, replayed identically against
// every implementation so their timings and outputs can be compared.

enum class EventType : std::uint8_t { Add, Cancel, Modify, Market };
inline constexpr std::size_t kEventTypes = 4;

struct Event {
  EventType type;
  Side side;
  OrderId id;
  Price price;
  Quantity qty;
};

template <OrderBook B>
inline void apply(B &book, const Event &e, std::vector<Trade> &out) {
  switch (e.type) {
  case EventType::Add:
    book.add_limit(e.id, e.side, e.price, e.qty, out);
    break;
  case EventType::Cancel:
    book.cancel(e.id);
    break;
  case EventType::Modify:
    book.modify(e.id, e.price, e.qty, out);
    break;
  case EventType::Market:
    book.add_market(e.id, e.side, e.qty, out);
    break;
  }
}

struct Workload {
  std::vector<Event> events;
  std::size_t warmup = 0; // events[0, warmup) build the book; not measured
};

struct WorkloadConfig {
  std::size_t events = 1'000'000;   // measured events after warmup
  std::size_t target_depth = 5'000; // resting orders the book hovers around
  std::uint64_t seed = 42;
};

// Generates a roughly market-like event mix: passive limit orders clustered a
// few ticks from a slowly drifting mid price, cancels and modifies of orders
// that are actually resting, and occasional market orders that take liquidity.
//
// To know which orders are live, the generator replays its own events through
// a MapOrderBook as it goes. Cancels and modifies therefore always target a
// resting order, as they would in a real feed.
class WorkloadGenerator {
public:
  explicit WorkloadGenerator(const WorkloadConfig &config)
      : config_(config), rng_(config.seed) {}

  Workload generate() {
    Workload w;
    w.events.reserve(config_.target_depth + config_.events);
    while (live_.size() < config_.target_depth)
      emit(w, make_add());
    w.warmup = w.events.size();

    for (std::size_t i = 0; i < config_.events; ++i) {
      drift_mid();
      emit(w, next_event());
    }
    return w;
  }

private:
  struct LiveOrder {
    Side side;
    Price price;
    Quantity qty;     // remaining
    std::size_t slot; // index in live_ids_
  };

  Event next_event() {
    const double r = unit_(rng_);
    if (r < 0.04)
      return make_market();
    if (live_ids_.empty())
      return make_add();
    if (r < 0.12)
      return make_modify();
    // Cancel more often when the book is deep so its size stays near target.
    const double cancel_share =
        live_.size() > config_.target_depth ? 0.50 : 0.36;
    if (r < 0.12 + cancel_share)
      return make_cancel();
    return make_add();
  }

  Event make_add() {
    const Side side = coin() ? Side::Buy : Side::Sell;
    return Event{EventType::Add, side, next_id_++, passive_price(side),
                 lots(1, 10)};
  }

  Event make_market() {
    const Side side = coin() ? Side::Buy : Side::Sell;
    return Event{EventType::Market, side, next_id_++, 0, lots(1, 5)};
  }

  Event make_cancel() {
    const OrderId id = random_live_id();
    return Event{EventType::Cancel, live_.at(id).side, id, 0, 0};
  }

  // Half reduce size in place (keeps priority), half reprice.
  Event make_modify() {
    const OrderId id = random_live_id();
    const LiveOrder &o = live_.at(id);
    if (coin()) {
      const Quantity reduced = o.qty > 1 ? o.qty / 2 : 1;
      return Event{EventType::Modify, o.side, id, o.price, reduced};
    }
    return Event{EventType::Modify, o.side, id, passive_price(o.side),
                 lots(1, 10)};
  }

  // Appends the event and replays it to keep the live-order set accurate.
  void emit(Workload &w, const Event &e) {
    w.events.push_back(e);

    bool in_place_reduce = false;
    if (e.type == EventType::Modify) {
      const LiveOrder &o = live_.at(e.id);
      in_place_reduce = e.price == o.price && e.qty <= o.qty;
    }

    trades_.clear();
    apply(book_, e, trades_);

    Quantity aggressor_filled = 0;
    for (const Trade &t : trades_) {
      aggressor_filled += t.qty;
      LiveOrder &resting = live_.at(t.resting);
      resting.qty -= t.qty;
      if (resting.qty == 0)
        remove_live(t.resting);
    }

    switch (e.type) {
    case EventType::Add:
      if (e.qty > aggressor_filled)
        add_live(e.id, e.side, e.price, e.qty - aggressor_filled);
      break;
    case EventType::Cancel:
      remove_live(e.id);
      break;
    case EventType::Modify:
      if (in_place_reduce) {
        live_.at(e.id).qty = e.qty;
      } else {
        remove_live(e.id);
        if (e.qty > aggressor_filled)
          add_live(e.id, e.side, e.price, e.qty - aggressor_filled);
      }
      break;
    case EventType::Market:
      break;
    }
  }

  void add_live(OrderId id, Side side, Price price, Quantity qty) {
    live_.emplace(id, LiveOrder{side, price, qty, live_ids_.size()});
    live_ids_.push_back(id);
  }

  // O(1) removal: move the last id into the removed slot.
  void remove_live(OrderId id) {
    auto it = live_.find(id);
    const std::size_t slot = it->second.slot;
    const OrderId moved = live_ids_.back();
    live_ids_[slot] = moved;
    live_.at(moved).slot = slot;
    live_ids_.pop_back();
    live_.erase(it);
  }

  OrderId random_live_id() {
    std::uniform_int_distribution<std::size_t> pick(0, live_ids_.size() - 1);
    return live_ids_[pick(rng_)];
  }

  // Passive prices sit at least one tick from mid, a geometric distance
  // further out (mean ~4 ticks), so most liquidity is near the top.
  Price passive_price(Side side) {
    const Price offset = 1 + depth_(rng_);
    return side == Side::Buy ? mid_ - offset : mid_ + offset;
  }

  void drift_mid() {
    if (unit_(rng_) < 0.002)
      mid_ += coin() ? 1 : -1;
  }

  Quantity lots(Quantity lo, Quantity hi) {
    return 100 * std::uniform_int_distribution<Quantity>(lo, hi)(rng_);
  }

  bool coin() { return (rng_() & 1) != 0; }

  WorkloadConfig config_;
  std::mt19937_64 rng_;
  std::uniform_real_distribution<double> unit_{0.0, 1.0};
  std::geometric_distribution<Price> depth_{0.25};

  MapOrderBook book_;
  std::vector<Trade> trades_;
  std::unordered_map<OrderId, LiveOrder> live_;
  std::vector<OrderId> live_ids_;
  OrderId next_id_ = 1;
  Price mid_ = 100'000;
};
