// Replays one generated workload against each order book and reports
// throughput plus per-operation latency percentiles.
//
// For stable numbers: build the release preset, pin to one core
// (taskset -c 2 ./lob_bench), and close other heavy programs.
//
// Hardware counters (branch and cache misses) come from a separate untimed
// pass, so they never perturb the throughput or latency numbers.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "map_order_book.hpp"
#include "perf_counters.hpp"
#include "pool_order_book.hpp"
#include "workload.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// --- Timing ------------------------------------------------------------------

// Per-operation timestamps. On x86 this reads the CPU's timestamp counter
// (TSC), which costs a few nanoseconds versus ~20ns+ for steady_clock. The
// fences stop the CPU reordering the measured work across the reads.
#if defined(__x86_64__)
inline std::uint64_t ticks_begin() {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

inline std::uint64_t ticks_end() {
  unsigned aux;
  const std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}
#else
inline std::uint64_t ticks_begin() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

inline std::uint64_t ticks_end() { return ticks_begin(); }
#endif

// Ticks per nanosecond, measured against steady_clock over ~200ms.
double calibrate_ticks_per_ns() {
  const auto c0 = Clock::now();
  const std::uint64_t t0 = ticks_begin();
  while (Clock::now() - c0 < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t t1 = ticks_end();
  const auto c1 = Clock::now();
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count();
  return static_cast<double>(t1 - t0) / static_cast<double>(ns);
}

// Median cost of an empty timed region; included in every latency sample.
std::uint64_t timer_overhead_ticks() {
  std::vector<std::uint64_t> samples(100'000);
  for (auto &s : samples) {
    const std::uint64_t t0 = ticks_begin();
    s = ticks_end() - t0;
  }
  std::nth_element(samples.begin(), samples.begin() + 50'000, samples.end());
  return samples[50'000];
}

// --- Output checksum ---------------------------------------------------------

// Folds every trade into a hash that is printed at the end. That stops the
// compiler optimizing the matching away, and lets you confirm different books
// produced identical trades on the same workload.
struct Checksum {
  std::uint64_t trades = 0;
  std::uint64_t volume = 0;
  std::uint64_t hash = 14695981039346656037ull; // FNV-1a offset basis

  void add(const std::vector<Trade> &ts) {
    for (const Trade &t : ts) {
      ++trades;
      volume += t.qty;
      mix(t.aggressor);
      mix(t.resting);
      mix(static_cast<std::uint64_t>(t.price));
      mix(t.qty);
    }
  }

  void mix(std::uint64_t v) { hash = (hash ^ v) * 1099511628211ull; }
};

// --- Runs --------------------------------------------------------------------

template <OrderBook B>
void replay_warmup(B &book, const Workload &w, std::vector<Trade> &trades) {
  for (std::size_t i = 0; i < w.warmup; ++i) {
    trades.clear();
    apply(book, w.events[i], trades);
  }
}

// Times the whole measured stream with no per-op timers, so timer overhead
// doesn't inflate the result. Returns mean nanoseconds per event.
template <OrderBook B> double throughput_run(const Workload &w, Checksum &sum) {
  B book;
  std::vector<Trade> trades;
  trades.reserve(1024);
  replay_warmup(book, w, trades);

  const auto start = Clock::now();
  for (std::size_t i = w.warmup; i < w.events.size(); ++i) {
    trades.clear();
    apply(book, w.events[i], trades);
    sum.add(trades);
  }
  const auto elapsed = Clock::now() - start;
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  return static_cast<double>(ns) /
         static_cast<double>(w.events.size() - w.warmup);
}

// Same replay as throughput_run, but with hardware counters enabled around
// the measured events instead of the clock. Kept as its own pass so the
// timed runs are byte-for-byte what they were without counters.
template <OrderBook B>
CounterValues counter_run(const Workload &w, PerfCounters &counters,
                          Checksum &sum) {
  B book;
  std::vector<Trade> trades;
  trades.reserve(1024);
  replay_warmup(book, w, trades);

  counters.start();
  for (std::size_t i = w.warmup; i < w.events.size(); ++i) {
    trades.clear();
    apply(book, w.events[i], trades);
    sum.add(trades);
  }
  return counters.stop();
}

using Samples = std::array<std::vector<std::uint64_t>, kEventTypes>;

// Times each event individually, bucketed by event type.
template <OrderBook B>
Samples latency_run(const Workload &w, Checksum &sum,
                    std::size_t &final_depth) {
  B book;
  std::vector<Trade> trades;
  trades.reserve(1024);
  replay_warmup(book, w, trades);

  Samples samples;
  for (auto &s : samples)
    s.reserve(w.events.size() - w.warmup);

  for (std::size_t i = w.warmup; i < w.events.size(); ++i) {
    const Event &e = w.events[i];
    trades.clear();
    const std::uint64_t t0 = ticks_begin();
    apply(book, e, trades);
    const std::uint64_t t1 = ticks_end();
    samples[static_cast<std::size_t>(e.type)].push_back(t1 - t0);
    sum.add(trades);
  }
  final_depth = book.order_count();
  return samples;
}

// --- Reporting ---------------------------------------------------------------

constexpr std::array<const char *, kEventTypes> kTypeNames = {
    "add", "cancel", "modify", "market"};

void print_latency_row(const char *name, std::vector<std::uint64_t> &ticks,
                       double ticks_per_ns) {
  if (ticks.empty())
    return;
  std::sort(ticks.begin(), ticks.end());
  auto pct = [&](double p) {
    const auto idx =
        static_cast<std::size_t>(p * static_cast<double>(ticks.size()));
    return static_cast<double>(ticks[std::min(idx, ticks.size() - 1)]) /
           ticks_per_ns;
  };
  std::printf("    %-8s %9zu %8.0f %8.0f %8.0f %8.0f %10.0f\n", name,
              ticks.size(), pct(0.50), pct(0.90), pct(0.99), pct(0.999),
              static_cast<double>(ticks.back()) / ticks_per_ns);
}

void print_counters(const CounterValues &c, double events) {
  std::printf("  counters (per event):");
  if (c.has(Counter::Cycles))
    std::printf(" %.1f cycles", c[Counter::Cycles] / events);
  if (c.has(Counter::Cycles) && c.has(Counter::Instructions))
    std::printf(", %.2f IPC", c[Counter::Instructions] / c[Counter::Cycles]);
  if (c.has(Counter::BranchMisses)) {
    std::printf(", %.3f branch-misses", c[Counter::BranchMisses] / events);
    if (c.has(Counter::Branches))
      std::printf(" (%.2f%%)",
                  100.0 * c[Counter::BranchMisses] / c[Counter::Branches]);
  }
  if (c.has(Counter::L1dMisses))
    std::printf(", %.3f L1d-misses", c[Counter::L1dMisses] / events);
  if (c.has(Counter::CacheMisses))
    std::printf(", %.3f cache-misses", c[Counter::CacheMisses] / events);
  if (c.multiplexed < 1.0)
    std::printf("  (multiplexed, %.0f%% live: estimates)",
                100.0 * c.multiplexed);
  std::printf("\n");
}

template <OrderBook B>
void run_book(const char *name, const Workload &w, int reps,
              double ticks_per_ns, PerfCounters &counters) {
  std::vector<double> ns_per_op;
  Checksum sum;
  for (int r = 0; r < reps; ++r) {
    sum = Checksum{};
    ns_per_op.push_back(throughput_run<B>(w, sum));
  }
  std::sort(ns_per_op.begin(), ns_per_op.end());
  const double median = ns_per_op[ns_per_op.size() / 2];

  Checksum latency_sum;
  std::size_t final_depth = 0;
  Samples samples = latency_run<B>(w, latency_sum, final_depth);

  Checksum counter_sum;
  CounterValues counter_values;
  if (counters.available())
    counter_values = counter_run<B>(w, counters, counter_sum);

  std::printf("\n%s\n", name);
  std::printf("  throughput: %.2f M events/s (%.1f ns/event, median of %d; "
              "min %.1f, max %.1f)\n",
              1e3 / median, median, reps, ns_per_op.front(), ns_per_op.back());
  std::printf("  latency (ns)   count      p50      p90      p99    p99.9"
              "        max\n");
  std::vector<std::uint64_t> all;
  for (const auto &s : samples)
    all.insert(all.end(), s.begin(), s.end());
  for (std::size_t t = 0; t < kEventTypes; ++t)
    print_latency_row(kTypeNames[t], samples[t], ticks_per_ns);
  print_latency_row("all", all, ticks_per_ns);
  if (counters.available())
    print_counters(counter_values,
                   static_cast<double>(w.events.size() - w.warmup));
  std::printf("  final depth: %zu orders\n", final_depth);
  std::printf("  checksum: %llu trades, %llu volume, hash %016llx%s\n",
              static_cast<unsigned long long>(sum.trades),
              static_cast<unsigned long long>(sum.volume),
              static_cast<unsigned long long>(sum.hash),
              latency_sum.hash == sum.hash &&
                      (!counters.available() || counter_sum.hash == sum.hash)
                  ? ""
                  : "  (MISMATCH between runs!)");
}

// --- Main --------------------------------------------------------------------

struct Options {
  WorkloadConfig workload;
  int reps = 5;
};

[[noreturn]] void usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s [--events N] [--depth N] [--seed N] [--reps N]\n"
               "  --events  measured events after warmup (default 1000000)\n"
               "  --depth   resting orders the book hovers around (default "
               "5000)\n"
               "  --seed    workload random seed (default 42)\n"
               "  --reps    throughput repetitions (default 5)\n",
               argv0);
  std::exit(1);
}

Options parse_args(int argc, char **argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (i + 1 >= argc)
      usage(argv[0]);
    const unsigned long long value = std::strtoull(argv[++i], nullptr, 10);
    if (arg == "--events")
      opt.workload.events = value;
    else if (arg == "--depth")
      opt.workload.target_depth = value;
    else if (arg == "--seed")
      opt.workload.seed = value;
    else if (arg == "--reps" && value > 0)
      opt.reps = static_cast<int>(value);
    else
      usage(argv[0]);
  }
  return opt;
}

} // namespace

int main(int argc, char **argv) {
  const Options opt = parse_args(argc, argv);

  const auto gen_start = Clock::now();
  const Workload w = WorkloadGenerator(opt.workload).generate();
  const auto gen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          Clock::now() - gen_start)
                          .count();

  std::array<std::size_t, kEventTypes> counts{};
  for (std::size_t i = w.warmup; i < w.events.size(); ++i)
    ++counts[static_cast<std::size_t>(w.events[i].type)];
  const double measured = static_cast<double>(w.events.size() - w.warmup);

  std::printf("workload: %zu warmup + %zu measured events (seed %llu, "
              "generated in %lld ms)\n",
              w.warmup, w.events.size() - w.warmup,
              static_cast<unsigned long long>(opt.workload.seed),
              static_cast<long long>(gen_ms));
  std::printf("  mix:");
  for (std::size_t t = 0; t < kEventTypes; ++t)
    std::printf(" %s %.1f%%", kTypeNames[t],
                100.0 * static_cast<double>(counts[t]) / measured);
  std::printf("\n");

  const double ticks_per_ns = calibrate_ticks_per_ns();
  std::printf("timer: %.3f ticks/ns, overhead ~%.0f ns per sample "
              "(included in latencies)\n",
              ticks_per_ns,
              static_cast<double>(timer_overhead_ticks()) / ticks_per_ns);

  PerfCounters counters;
  if (!counters.available())
    std::printf("counters: unavailable (no PMU access; check "
                "/proc/sys/kernel/perf_event_paranoid)\n");

  run_book<MapOrderBook>("MapOrderBook", w, opt.reps, ticks_per_ns, counters);
  run_book<PoolOrderBook>("PoolOrderBook", w, opt.reps, ticks_per_ns,
                          counters);
  return 0;
}
