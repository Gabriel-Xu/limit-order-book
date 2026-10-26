#pragma once

#include "flat_order_book.hpp"
#include "map_order_book.hpp"
#include "naive_order_book.hpp"
#include "pool_order_book.hpp"
#include "types.hpp"

#include <ostream>

#include <gtest/gtest.h>

// Every implementation runs the unit tests, including the naive oracle so the
// reference itself is checked. Add each new book to both lists.
using AllBooks = ::testing::Types<NaiveOrderBook, MapOrderBook, PoolOrderBook,
                                  FlatOrderBook>;

// Books differentially tested against NaiveOrderBook.
using BooksUnderTest =
    ::testing::Types<MapOrderBook, PoolOrderBook, FlatOrderBook>;

// Printers so GoogleTest failure messages show readable values.
inline std::ostream &operator<<(std::ostream &os, Side side) {
  return os << (side == Side::Buy ? "Buy" : "Sell");
}

inline std::ostream &operator<<(std::ostream &os, const Trade &t) {
  return os << "{aggressor " << t.aggressor << ", resting " << t.resting
            << ", price " << t.price << ", qty " << t.qty << "}";
}
