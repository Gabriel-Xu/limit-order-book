#include "test_support.hpp"

#include <vector>

#include <gtest/gtest.h>

namespace {

using Trades = std::vector<Trade>;

// Each helper clears `trades` first, so after a call it holds only the fills
// from that one operation.
template <typename B>
class OrderBookTest : public ::testing::Test {
protected:
  bool limit(OrderId id, Side side, Price price, Quantity qty) {
    trades.clear();
    return book.add_limit(id, side, price, qty, trades);
  }

  void market(OrderId id, Side side, Quantity qty) {
    trades.clear();
    book.add_market(id, side, qty, trades);
  }

  bool modify(OrderId id, Price price, Quantity qty) {
    trades.clear();
    return book.modify(id, price, qty, trades);
  }

  B book;
  Trades trades;
};

TYPED_TEST_SUITE(OrderBookTest, AllBooks);

// --- Resting and rejection ---------------------------------------------------

TYPED_TEST(OrderBookTest, StartsEmpty) {
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_FALSE(this->book.best_ask());
  EXPECT_EQ(this->book.order_count(), 0u);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 100), 0u);
}

TYPED_TEST(OrderBookTest, NonCrossingOrdersRest) {
  EXPECT_TRUE(this->limit(1, Side::Buy, 99, 10));
  EXPECT_TRUE(this->limit(2, Side::Buy, 98, 5));
  EXPECT_TRUE(this->limit(3, Side::Sell, 101, 7));
  EXPECT_TRUE(this->limit(4, Side::Sell, 102, 3));
  EXPECT_TRUE(this->trades.empty());

  EXPECT_EQ(this->book.best_bid(), 99);
  EXPECT_EQ(this->book.best_ask(), 101);
  EXPECT_EQ(this->book.order_count(), 4u);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 98), 5u);
  EXPECT_EQ(this->book.volume_at(Side::Sell, 102), 3u);
  EXPECT_EQ(this->book.volume_at(Side::Sell, 99), 0u); // wrong side
}

TYPED_TEST(OrderBookTest, OrdersAtSamePriceAggregate) {
  this->limit(1, Side::Buy, 99, 10);
  this->limit(2, Side::Buy, 99, 5);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 99), 15u);
  EXPECT_EQ(this->book.order_count(), 2u);
}

TYPED_TEST(OrderBookTest, RejectsZeroQuantity) {
  EXPECT_FALSE(this->limit(1, Side::Buy, 100, 0));
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, RejectsDuplicateRestingId) {
  EXPECT_TRUE(this->limit(1, Side::Buy, 99, 10));
  EXPECT_FALSE(this->limit(1, Side::Sell, 105, 5));
  EXPECT_EQ(this->book.order_count(), 1u);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 99), 10u);
  EXPECT_EQ(this->book.volume_at(Side::Sell, 105), 0u);
}

TYPED_TEST(OrderBookTest, IdCanBeReusedAfterFullFill) {
  this->limit(1, Side::Sell, 100, 5);
  this->limit(2, Side::Buy, 100, 5);
  EXPECT_TRUE(this->limit(1, Side::Buy, 99, 3));
  EXPECT_EQ(this->book.best_bid(), 99);
}

// --- Matching ----------------------------------------------------------------

TYPED_TEST(OrderBookTest, CrossingLimitTradesAtRestingPrice) {
  this->limit(1, Side::Sell, 101, 10);
  EXPECT_TRUE(this->limit(2, Side::Buy, 105, 10));
  EXPECT_EQ(this->trades, (Trades{{2, 1, 101, 10}}));
  EXPECT_EQ(this->book.order_count(), 0u);
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_FALSE(this->book.best_ask());
}

TYPED_TEST(OrderBookTest, PartialFillRestsAggressorRemainder) {
  this->limit(1, Side::Sell, 101, 4);
  this->limit(2, Side::Buy, 101, 10);
  EXPECT_EQ(this->trades, (Trades{{2, 1, 101, 4}}));
  EXPECT_FALSE(this->book.best_ask());
  EXPECT_EQ(this->book.best_bid(), 101);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 101), 6u);
}

TYPED_TEST(OrderBookTest, PartialFillLeavesRestingRemainder) {
  this->limit(1, Side::Sell, 101, 10);
  this->limit(2, Side::Buy, 101, 3);
  EXPECT_EQ(this->trades, (Trades{{2, 1, 101, 3}}));
  EXPECT_EQ(this->book.volume_at(Side::Sell, 101), 7u);
  EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, FifoWithinPriceLevel) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 101, 5);
  this->limit(3, Side::Sell, 101, 5);

  this->limit(4, Side::Buy, 101, 7);
  EXPECT_EQ(this->trades, (Trades{{4, 1, 101, 5}, {4, 2, 101, 2}}));

  this->limit(5, Side::Buy, 101, 3);
  EXPECT_EQ(this->trades, (Trades{{5, 2, 101, 3}}));

  this->limit(6, Side::Buy, 101, 1);
  EXPECT_EQ(this->trades, (Trades{{6, 3, 101, 1}}));
  EXPECT_EQ(this->book.volume_at(Side::Sell, 101), 4u);
}

TYPED_TEST(OrderBookTest, BuySweepsAsksLowestFirst) {
  this->limit(1, Side::Sell, 103, 5);
  this->limit(2, Side::Sell, 101, 5);
  this->limit(3, Side::Sell, 102, 5);

  this->limit(4, Side::Buy, 103, 12);
  EXPECT_EQ(this->trades,
            (Trades{{4, 2, 101, 5}, {4, 3, 102, 5}, {4, 1, 103, 2}}));
  EXPECT_EQ(this->book.best_ask(), 103);
  EXPECT_EQ(this->book.volume_at(Side::Sell, 103), 3u);
}

TYPED_TEST(OrderBookTest, SellSweepsBidsHighestFirst) {
  this->limit(1, Side::Buy, 98, 5);
  this->limit(2, Side::Buy, 100, 5);
  this->limit(3, Side::Buy, 99, 5);

  this->limit(4, Side::Sell, 99, 12);
  EXPECT_EQ(this->trades, (Trades{{4, 2, 100, 5}, {4, 3, 99, 5}}));
  EXPECT_EQ(this->book.best_ask(), 99); // remainder rests
  EXPECT_EQ(this->book.volume_at(Side::Sell, 99), 2u);
  EXPECT_EQ(this->book.best_bid(), 98);
}

TYPED_TEST(OrderBookTest, LimitStopsAtLimitPrice) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 102, 5);

  this->limit(3, Side::Buy, 101, 8);
  EXPECT_EQ(this->trades, (Trades{{3, 1, 101, 5}}));
  EXPECT_EQ(this->book.best_bid(), 101);
  EXPECT_EQ(this->book.volume_at(Side::Buy, 101), 3u);
  EXPECT_EQ(this->book.best_ask(), 102);
}

// --- Market orders -----------------------------------------------------------

TYPED_TEST(OrderBookTest, MarketBuySweepsAndDiscardsRemainder) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 102, 5);

  this->market(3, Side::Buy, 15);
  EXPECT_EQ(this->trades, (Trades{{3, 1, 101, 5}, {3, 2, 102, 5}}));
  EXPECT_EQ(this->book.order_count(), 0u);
  EXPECT_FALSE(this->book.best_bid()); // remainder did not rest
}

TYPED_TEST(OrderBookTest, MarketSellPartiallyFillsBid) {
  this->limit(1, Side::Buy, 100, 5);
  this->market(2, Side::Sell, 3);
  EXPECT_EQ(this->trades, (Trades{{2, 1, 100, 3}}));
  EXPECT_EQ(this->book.volume_at(Side::Buy, 100), 2u);
}

TYPED_TEST(OrderBookTest, MarketOnEmptyBookDoesNothing) {
  this->market(1, Side::Buy, 10);
  EXPECT_TRUE(this->trades.empty());
  EXPECT_EQ(this->book.order_count(), 0u);
}

// --- Cancel ------------------------------------------------------------------

TYPED_TEST(OrderBookTest, CancelRemovesOrder) {
  this->limit(1, Side::Buy, 99, 10);
  EXPECT_TRUE(this->book.cancel(1));
  EXPECT_EQ(this->book.order_count(), 0u);
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_FALSE(this->book.cancel(1)); // already gone
}

TYPED_TEST(OrderBookTest, CancelUnknownIdFails) {
  EXPECT_FALSE(this->book.cancel(42));
}

TYPED_TEST(OrderBookTest, CancelFilledOrderFails) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Buy, 101, 5);
  EXPECT_FALSE(this->book.cancel(1));
}

TYPED_TEST(OrderBookTest, CancelMiddleOfQueuePreservesOthersOrder) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 101, 5);
  this->limit(3, Side::Sell, 101, 5);
  EXPECT_TRUE(this->book.cancel(2));

  this->limit(4, Side::Buy, 101, 10);
  EXPECT_EQ(this->trades, (Trades{{4, 1, 101, 5}, {4, 3, 101, 5}}));
}

TYPED_TEST(OrderBookTest, CancelLastOrderAtBestUpdatesBest) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 102, 5);
  this->book.cancel(1);
  EXPECT_EQ(this->book.best_ask(), 102);
}

// --- Modify ------------------------------------------------------------------

TYPED_TEST(OrderBookTest, ModifyReduceKeepsPriority) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 101, 5);
  EXPECT_TRUE(this->modify(1, 101, 3));
  EXPECT_TRUE(this->trades.empty());
  EXPECT_EQ(this->book.volume_at(Side::Sell, 101), 8u);

  this->limit(3, Side::Buy, 101, 4);
  EXPECT_EQ(this->trades, (Trades{{3, 1, 101, 3}, {3, 2, 101, 1}}));
}

TYPED_TEST(OrderBookTest, ModifyIncreaseLosesPriority) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 101, 5);
  EXPECT_TRUE(this->modify(1, 101, 6));

  this->limit(3, Side::Buy, 101, 5);
  EXPECT_EQ(this->trades, (Trades{{3, 2, 101, 5}}));
  EXPECT_EQ(this->book.volume_at(Side::Sell, 101), 6u);
}

TYPED_TEST(OrderBookTest, ModifyPriceLosesPriority) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 101, 5);
  EXPECT_TRUE(this->modify(1, 102, 5));
  EXPECT_TRUE(this->modify(1, 101, 5)); // back at 101, now behind order 2

  this->limit(3, Side::Buy, 101, 5);
  EXPECT_EQ(this->trades, (Trades{{3, 2, 101, 5}}));
}

TYPED_TEST(OrderBookTest, ModifyToCrossingPriceTrades) {
  this->limit(1, Side::Buy, 99, 5);
  this->limit(2, Side::Sell, 101, 5);

  EXPECT_TRUE(this->modify(1, 101, 3));
  EXPECT_EQ(this->trades, (Trades{{1, 2, 101, 3}}));
  EXPECT_FALSE(this->book.best_bid());
  EXPECT_EQ(this->book.volume_at(Side::Sell, 101), 2u);
  EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, ModifyToZeroCancels) {
  this->limit(1, Side::Buy, 99, 5);
  EXPECT_TRUE(this->modify(1, 99, 0));
  EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(OrderBookTest, ModifyUnknownIdFails) {
  EXPECT_FALSE(this->modify(7, 100, 5));
}

// --- Output contract ---------------------------------------------------------

TYPED_TEST(OrderBookTest, TradesAreAppendedNotCleared) {
  const Trade sentinel{0, 0, 0, 0};
  Trades out{sentinel};
  this->book.add_limit(1, Side::Sell, 101, 5, out);
  this->book.add_limit(2, Side::Buy, 101, 5, out);
  EXPECT_EQ(out, (Trades{sentinel, {2, 1, 101, 5}}));
}

} // namespace
