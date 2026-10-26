#include "test_support.hpp"
#include "tuned_order_book.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

// Tests for the books that index levels by price offset and orders by id
// (FlatOrderBook, TunedOrderBook). These cover what the shared tests can't:
// a configured price range, id-index growth, and sparse books where the next
// best level is far away, which is where the walk (Flat) or the bitmap's
// cross-word search (Tuned) actually runs.

namespace {

using Trades = std::vector<Trade>;

template <typename B>
class LadderBookTest : public ::testing::Test {};
TYPED_TEST_SUITE(LadderBookTest, LadderBooks);

TYPED_TEST(LadderBookTest, RejectsLimitOutsidePriceRange) {
  TypeParam book(100, 200);
  Trades trades;
  EXPECT_FALSE(book.add_limit(1, Side::Buy, 99, 10, trades));
  EXPECT_FALSE(book.add_limit(2, Side::Sell, 201, 10, trades));
  EXPECT_TRUE(book.add_limit(3, Side::Buy, 100, 10, trades));
  EXPECT_TRUE(book.add_limit(4, Side::Sell, 200, 10, trades));
  EXPECT_EQ(book.order_count(), 2u);
  EXPECT_EQ(book.volume_at(Side::Buy, 99), 0u);
  EXPECT_EQ(book.volume_at(Side::Sell, 201), 0u);
  EXPECT_TRUE(trades.empty());
}

TYPED_TEST(LadderBookTest, ModifyOutsidePriceRangeLeavesOrderResting) {
  TypeParam book(100, 200);
  Trades trades;
  ASSERT_TRUE(book.add_limit(1, Side::Buy, 150, 10, trades));
  EXPECT_FALSE(book.modify(1, 201, 10, trades));
  EXPECT_EQ(book.order_count(), 1u);
  EXPECT_EQ(book.volume_at(Side::Buy, 150), 10u);
  EXPECT_EQ(book.best_bid(), 150);
}

TYPED_TEST(LadderBookTest, ExtremePricesOfRange) {
  TypeParam book(-500, 500);
  Trades trades;
  ASSERT_TRUE(book.add_limit(1, Side::Buy, -500, 3, trades));
  ASSERT_TRUE(book.add_limit(2, Side::Sell, 500, 4, trades));
  EXPECT_EQ(book.best_bid(), -500);
  EXPECT_EQ(book.best_ask(), 500);
  book.add_market(3, Side::Buy, 10, trades);
  EXPECT_EQ(trades, (Trades{{3, 2, 500, 4}}));
  EXPECT_FALSE(book.best_ask());
  EXPECT_EQ(book.best_bid(), -500);
}

TYPED_TEST(LadderBookTest, IdIndexGrowsPastInitialSize) {
  TypeParam book(0, 1000, /*order_count=*/4, /*capacity=*/2);
  Trades trades;
  for (OrderId id = 0; id < 100; ++id)
    ASSERT_TRUE(book.add_limit(id, Side::Buy, 500, 1, trades));
  ASSERT_TRUE(book.add_limit(1'000'000, Side::Buy, 400, 7, trades));
  EXPECT_EQ(book.order_count(), 101u);
  EXPECT_FALSE(book.add_limit(1'000'000, Side::Buy, 400, 7, trades));
  EXPECT_TRUE(book.cancel(1'000'000));
  EXPECT_TRUE(book.cancel(99));
  EXPECT_FALSE(book.cancel(99));
  EXPECT_FALSE(book.cancel(2'000'000)); // past the index entirely
  EXPECT_EQ(book.order_count(), 99u);
  EXPECT_EQ(book.volume_at(Side::Buy, 500), 99u);
}

// Few resting orders spread over a wide range, so emptying the best level
// usually means searching far for the next one: across many bitmap words and
// summary words. Compared step by step against the naive reference.
TYPED_TEST(LadderBookTest, MatchesNaiveReferenceOnSparseBook) {
  constexpr Price kMin = 0;
  constexpr Price kMax = 300'000; // 4688 bitmap words, 74 summary words
  constexpr int kOps = 4000;

  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    TypeParam book(kMin, kMax);
    NaiveOrderBook ref;
    std::mt19937_64 rng(seed);
    auto uniform = [&]<typename T>(T lo, T hi) {
      return std::uniform_int_distribution<T>(lo, hi)(rng);
    };
    // Mostly anywhere in the range, sometimes pinned to an edge.
    auto price = [&] {
      const int r = uniform(0, 19);
      return r == 0 ? kMin : r == 1 ? kMax : uniform(kMin, kMax);
    };

    Trades got;
    Trades want;
    OrderId next_id = 1;
    for (int step = 0; step < kOps; ++step) {
      SCOPED_TRACE(::testing::Message()
                   << "seed " << seed << ", step " << step);
      got.clear();
      want.clear();

      const OrderId recent = uniform(next_id > 40 ? next_id - 40 : 1, next_id);
      const Side side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
      const Price p = price();
      const int op = uniform(0, 99);

      if (op < 50) {
        const Quantity qty = uniform(Quantity{1}, Quantity{20});
        ASSERT_EQ(book.add_limit(next_id, side, p, qty, got),
                  ref.add_limit(next_id, side, p, qty, want));
        ++next_id;
      } else if (op < 60) {
        const Quantity qty = uniform(Quantity{1}, Quantity{60});
        book.add_market(next_id, side, qty, got);
        ref.add_market(next_id, side, qty, want);
        ++next_id;
      } else if (op < 85) {
        ASSERT_EQ(book.cancel(recent), ref.cancel(recent));
      } else {
        const Quantity qty = uniform(Quantity{0}, Quantity{20});
        ASSERT_EQ(book.modify(recent, p, qty, got),
                  ref.modify(recent, p, qty, want));
      }

      ASSERT_EQ(got, want);
      ASSERT_EQ(book.best_bid(), ref.best_bid());
      ASSERT_EQ(book.best_ask(), ref.best_ask());
      ASSERT_EQ(book.order_count(), ref.order_count());
      for (Side s : {Side::Buy, Side::Sell})
        ASSERT_EQ(book.volume_at(s, p), ref.volume_at(s, p));
    }
  }
}

TEST(TunedOrderBookTest, RejectsLimitIdsThatDoNotFit32Bits) {
  TunedOrderBook book;
  Trades trades;
  const OrderId too_big = OrderId{UINT32_MAX} + 1;
  EXPECT_FALSE(book.add_limit(too_big, Side::Buy, 100, 1, trades));
  EXPECT_FALSE(book.cancel(too_big));
  EXPECT_EQ(book.order_count(), 0u);
  // Market ids never rest, so they are not limited.
  ASSERT_TRUE(book.add_limit(1, Side::Sell, 100, 1, trades));
  book.add_market(too_big, Side::Buy, 1, trades);
  EXPECT_EQ(trades, (Trades{{too_big, 1, 100, 1}}));
}

} // namespace
