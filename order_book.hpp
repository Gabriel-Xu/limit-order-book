#pragma once

#include "types.hpp"

#include <concepts>
#include <cstddef>
#include <optional>
#include <vector>

// Interface every order book implementation must satisfy.
//
// Implementations are unrelated classes checked against this concept rather
// than children of a virtual base, so calls resolve at compile time and can be
// inlined. Tests and benchmarks are templates over `OrderBook B`.
//
// Semantics (shared by all implementations):
//   add_limit  Match against the opposite side at prices at least as good as
//              `price`, then rest any remainder. Returns false (and does
//              nothing) if `qty` is 0 or `id` is already resting.
//   add_market Match against the opposite side at any price; any unfilled
//              remainder is discarded (never rests).
//   cancel     Remove a resting order. Returns false if `id` is not resting.
//   modify     Change a resting order's price and remaining quantity.
//              Same price with qty <= current keeps queue priority; anything
//              else is cancel + re-add (loses priority, may trade). qty 0
//              cancels. Returns false if `id` is not resting.
//
// Trades are appended to the caller-owned `out` vector so the hot path never
// has to allocate a fresh container.
template <typename B>
concept OrderBook =
    requires(B b, const B cb, OrderId id, Side side, Price price, Quantity qty,
             std::vector<Trade> &out) {
      { b.add_limit(id, side, price, qty, out) } -> std::same_as<bool>;
      { b.add_market(id, side, qty, out) } -> std::same_as<void>;
      { b.cancel(id) } -> std::same_as<bool>;
      { b.modify(id, price, qty, out) } -> std::same_as<bool>;

      { cb.best_bid() } -> std::same_as<std::optional<Price>>;
      { cb.best_ask() } -> std::same_as<std::optional<Price>>;
      { cb.volume_at(side, price) } -> std::same_as<Quantity>;
      { cb.order_count() } -> std::same_as<std::size_t>;
    };
