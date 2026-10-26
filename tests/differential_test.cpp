#include "test_support.hpp"

#include <cstdint>
#include <random>
#include <vector>

#include <gtest/gtest.h>

namespace {

// A narrow price band and small quantities keep the book dense, so random
// operations constantly hit partial fills, multi-level sweeps and emptied
// levels instead of resting far apart.
constexpr Price kMinPrice = 95;
constexpr Price kMaxPrice = 105;
constexpr Quantity kMaxQty = 20;

constexpr std::uint64_t kSeeds = 50;
constexpr int kOpsPerSeed = 10'000;

template <typename B>
class DifferentialTest : public ::testing::Test {};
TYPED_TEST_SUITE(DifferentialTest, BooksUnderTest);

// Compares every observable piece of state. Queue order isn't directly
// observable, but a FIFO bug shows up as a trade mismatch on a later fill.
template <typename B>
void expect_same_state(const B &book, const NaiveOrderBook &ref) {
  ASSERT_EQ(book.best_bid(), ref.best_bid());
  ASSERT_EQ(book.best_ask(), ref.best_ask());
  ASSERT_EQ(book.order_count(), ref.order_count());
  for (Price p = kMinPrice - 1; p <= kMaxPrice + 1; ++p) {
    for (Side side : {Side::Buy, Side::Sell}) {
      ASSERT_EQ(book.volume_at(side, p), ref.volume_at(side, p))
          << "at " << side << " " << p;
    }
  }
  if (book.best_bid() && book.best_ask()) {
    ASSERT_LT(*book.best_bid(), *book.best_ask()) << "book is crossed";
  }
}

TYPED_TEST(DifferentialTest, MatchesNaiveReferenceOnRandomOperations) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    TypeParam book;
    NaiveOrderBook ref;
    std::mt19937_64 rng(seed);
    auto uniform = [&]<typename T>(T lo, T hi) {
      return std::uniform_int_distribution<T>(lo, hi)(rng);
    };

    std::vector<Trade> got;
    std::vector<Trade> want;
    OrderId next_id = 1;

    for (int step = 0; step < kOpsPerSeed; ++step) {
      SCOPED_TRACE(::testing::Message()
                   << "seed " << seed << ", step " << step);
      got.clear();
      want.clear();

      // Cancels and modifies target recently issued ids: most are still
      // resting, some were filled or cancelled, and next_id is never issued.
      const OrderId oldest = next_id > 50 ? next_id - 50 : 1;
      const OrderId recent = uniform(oldest, next_id);
      const Side side = uniform(0, 1) == 0 ? Side::Buy : Side::Sell;
      const Price price = uniform(kMinPrice, kMaxPrice);
      const int op = uniform(0, 99);

      if (op < 45) {
        // A few adds reuse a recent id to exercise duplicate rejection.
        const OrderId id = op < 5 ? recent : next_id++;
        const Quantity qty = uniform(Quantity{0}, kMaxQty);
        ASSERT_EQ(book.add_limit(id, side, price, qty, got),
                  ref.add_limit(id, side, price, qty, want));
      } else if (op < 55) {
        const OrderId id = next_id++;
        const Quantity qty = uniform(Quantity{1}, 3 * kMaxQty);
        book.add_market(id, side, qty, got);
        ref.add_market(id, side, qty, want);
      } else if (op < 75) {
        ASSERT_EQ(book.cancel(recent), ref.cancel(recent));
      } else {
        const Quantity qty = uniform(Quantity{0}, kMaxQty);
        ASSERT_EQ(book.modify(recent, price, qty, got),
                  ref.modify(recent, price, qty, want));
      }

      ASSERT_EQ(got, want);
      ASSERT_NO_FATAL_FAILURE(expect_same_state(book, ref));
    }
  }
}

} // namespace
