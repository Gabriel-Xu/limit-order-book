#pragma once

#include <cstdint>

// Vocabulary types shared by every order book implementation.

using OrderId = std::uint64_t;
using Price = std::int64_t; // integer ticks, never floating point
using Quantity = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

// One fill between an incoming (aggressor) order and a resting order.
// Trades always execute at the resting order's price.
struct Trade {
  OrderId aggressor;
  OrderId resting;
  Price price;
  Quantity qty;

  friend bool operator==(const Trade &, const Trade &) = default;
};
