# limit-order-book

A single-instrument limit order book and matching engine in C++20, with
price-time priority, limit/market/cancel/modify orders, and several
implementations benchmarked against each other.

## Build and run

Requires CMake 3.25+ and a C++20 compiler. GoogleTest is downloaded on the
first configure.

```sh
# Tests, under AddressSanitizer + UBSan
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug

# Benchmark (optimized, -march=native), pinned to one core
cmake --preset release
cmake --build --preset release -j
taskset -c 2 ./build/release/lob_bench [--events N] [--depth N] [--seed N] [--reps N]
```

The `profile` preset builds optimized code with debug info for `perf record`.

On Linux the benchmark also prints hardware counters per event (cycles, IPC,
branch misses, L1d read misses, and the kernel's generic `cache-misses`, which
is LLC misses on Intel and roughly L2 misses on AMD Zen). They come from an
extra untimed replay with counters enabled only around the measured events,
so the throughput and latency numbers are unaffected. They need
`perf_event_paranoid` <= 2 (the default) and a PMU the kernel can see; inside a
VM without one, the bench says so and skips them.

## Layout

| Path | Contents |
|---|---|
| `types.hpp` | Shared types: `OrderId`, `Price` (integer ticks), `Quantity`, `Side`, `Trade` |
| `order_book.hpp` | The `OrderBook` concept and the matching rules every implementation follows |
| `map_order_book.hpp` | Baseline: `std::map` of price levels, `std::list` FIFO queues, hash-map order index |
| `pool_order_book.hpp` | Improved implementation with pooling and intrusive doubly linked lists |
| `flat_order_book.hpp` | Implementation now uses flat data structures (`std::vector`) over maps |
| `tuned_order_book.hpp` | Flat book plus a level bitmap, 16-byte orders, 8-byte index entries and huge pages |
| `tests/naive_order_book.hpp` | Linear-scan reference book used as the test oracle |
| `tests/order_book_test.cpp` | Unit tests, run against every implementation |
| `tests/ladder_book_test.cpp` | Price-range, id-growth and sparse-book tests for the flat and tuned books |
| `tests/differential_test.cpp` | Random operations compared step by step against the reference book |
| `bench/workload.hpp` | Deterministic synthetic order flow generator |
| `bench/bench.cpp` | Throughput, per-operation latency percentiles, and hardware counters |
| `bench/perf_counters.hpp` | `perf_event_open` wrapper for cycle, branch-miss and cache-miss counts |

## Implementations

Each book is a step from the previous one, so the benchmark isolates what each
change buys. All of them produce identical trades on the same input (the
benchmark checksum confirms it).

| Book | Price levels | Orders in a level | Id → order | What it removes |
|---|---|---|---|---|
| `MapOrderBook` | `std::map` (red-black tree) | `std::list` | `std::unordered_map` | Reference design; obviously correct |
| `PoolOrderBook` | `std::map` | Intrusive doubly linked list of indices into one `std::vector<Order>` pool, with a free list | `std::unordered_map` | A heap allocation per order and the list's scattered nodes |
| `FlatOrderBook` | One `std::vector<Level>` per side, indexed by `price - min_price`, plus a cached best price | Same as Pool | `std::vector` indexed by id | Tree walks and hashing: every lookup is one array index |
| `TunedOrderBook` | Flat ladder plus a two-level bitmap of non-empty levels | Same, with 16-byte orders | 8-byte index entries | Remaining cache and TLB misses, and the linear walk for the next best price |

Notes on individual designs:

- **Pool** links orders by 32-bit pool index rather than pointer. Indices stay
  valid when the pool's vector reallocates, so the pool grows on demand, and
  they are half the size of pointers. A freed slot reuses its `next` link as
  the free-list link.
- **Flat** tracks each side's best price and a count of non-empty levels. When
  the best level empties it walks toward worse prices to the next non-empty
  level; the count lets an emptied side skip the walk. The walk is short in a
  dense book but O(price range) in a sparse one.
- **Tuned** replaces that walk with a bitmap (one bit per level) plus a
  summary bitmap (one bit per 64-bit word), so the next level is one or two
  `countl_zero`/`countr_zero` scans. Orders shrink from 24 to 16 bytes (the
  id is stored as 32 bits, four orders per cache line), index entries from 24
  to 8 bytes (pool slot plus price offset, side packed in the top bit), and
  the large arrays are 2 MiB aligned and `madvise(MADV_HUGEPAGE)`d.
  `Order` is deliberately **not** split into hot and cold fields: matching
  reads id, qty and next, and cancel reads next and prev, so a split would add
  a second cache line per order rather than save one.

## Benchmark results

Workload: 5,000 warmup adds, then 1,000,000 measured events (seed 42) at about
52% add, 36% cancel, 8% modify and 4% market. Passive orders rest 1 + Geometric(0.25)
ticks from a mid price that drifts by one tick on 0.2% of events, so most
liquidity is in the top few levels and the book holds about 5,000 orders.
Cancels and modifies always target a resting order, chosen uniformly.

Machine: AMD Ryzen 7 PRO 7840U (Zen 4) laptop, WSL2 (Linux 6.18), g++ 15.2,
`-O3 -march=native`, pinned with `taskset -c 2`. Values are medians of 3 runs;
throughput within a run is the median of 5 repetitions.

| Book | ns/event | cycles/event | IPC | branch misses/event | L1d misses/event | cache misses/event |
|---|---|---|---|---|---|---|
| Map | 53.8 | 251 | 1.74 | 3.38 | 6.39 | 0.211 |
| Pool | 48.2 | 228 | 1.76 | 3.21 | 5.95 | 0.146 |
| Flat | 17.1 | 82 | 2.19 | 1.22 | 2.79 | 0.109 |
| Tuned | 18.1 | **73** | 2.15 | 1.21 | **2.22** | **0.045** |

Per-event latency over all event types, in ns (includes about 20 ns of timer
overhead; see the measurement notes):

| Book | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| Map | 70 | 100 | 210 | 641 |
| Pool | 60 | 90 | 190 | 581 |
| Flat | 30 | 50 | 160 | 351 |
| Tuned | 30 | 40 | **80** | **200** |

Max latency is 30 to 300 µs for every book. That comes from the environment,
not the code (see below), so it is left out of the table.

### Why the numbers move

- **Map → Pool (about 9% fewer cycles).** Pooling removes the `malloc`/`free`
  pair per order and keeps orders contiguous, so L1d misses drop slightly
  (6.4 → 5.9). The gain is modest because the per-event cost is still
  dominated by the two structures Pool kept: a `std::map` lookup (a chain of
  dependent pointer loads through the tree) and an `unordered_map` lookup
  (hash, then a bucket chain).
- **Pool → Flat (about 2.8× fewer cycles).** Both lookups become a single
  array index, which removes:
  - the dependent loads, so L1d misses fall 5.9 → 2.8;
  - most branch misses (3.2 → 1.2 per event), because the tree's
    comparisons at each node are data-dependent and unpredictable while
    indexing has no branches.

  Fewer stalls lets the core run more instructions per cycle: IPC goes from
  1.76 to 2.19.
- **Flat → Tuned (about 11% fewer cycles, tail latency halved).** In this dense
  workload the best-price walk was already short, so the bitmap barely
  changes the average. The improvement comes from memory footprint:
  - The id index shrinks from 24 MB to 8 MB.
  - Orders pack four to a cache line.
  - Huge pages turn thousands of 4 KiB TLB entries into a handful.

  Cache misses fall 2.4× (0.109 → 0.045). The slowest operations are the ones
  that miss cache, so p99 halves (160 → 80 ns) and p99.9 drops 351 → 200 ns.
  The bitmap matters in sparse books instead: the sparse-book test takes
  9.1 s for Flat and 1.4 s for Tuned in the debug build.
- **Why Tuned's ns/event isn't lower than Flat's here.** It uses fewer cycles,
  but the benchmark runs it last. By then this laptop's clock has dropped from
  about 4.6 GHz towards 3.4 GHz under sustained load. When Tuned runs first it
  measures about 13 ns/event against Flat's 17. Compare books by cycles.

### Measurement notes

- **Timer resolution is about 10 ns.** On AMD Zen the TSC ticks at
  3.294 GHz but advances in steps of 32 to 33 ticks (it is updated at 100 MHz),
  so every latency sample is a multiple of about 10.02 ns. That is why
  latencies end in 0 or 1. Differences of one step (for example p50 of 30 vs
  40) are within the timer's resolution. Each sample also includes about 20 ns
  of timer overhead.
- **Max latency is host preemption.** A tight loop pinned to one core sees
  about 1,000 gaps longer than 2 µs per second (1.8% of CPU time). Most are
  invisible to Linux, so they are the Windows host descheduling the WSL2
  virtual CPU. A latency run lasts about 50 to 80 ms, so two or three events
  per run land on a gap of 50 µs or more. Tuning inside WSL2 (`isolcpus`,
  `nohz_full`, `SCHED_FIFO`) cannot remove that; bare-metal Linux with an
  isolated core, fixed clock frequency and IRQ affinity can.
- **ns/event depends on clock speed and run order.** Clock speed varied from
  3.1 to 4.6 GHz between runs on this machine. Occasionally one book's
  repetitions spread widely (for example 19.5 to 45.7 ns/event) while its
  cycle count and per-event percentiles stay normal. That points to a few long
  pauses outside the process, not slower code. Cycle counts don't depend on
  clock speed, so they are the reliable comparison.
- **The counters measure an extra, separate replay.** That keeps the timed
  runs identical to a build without counters. The counted region still
  includes the checksum hashing, the same as the throughput run.
- **The workload is synthetic.** It is dense near the top of book, so it
  favours designs that are fast there (the flat ladders) and hides the cost of
  a sparse book (Flat's walk), which the ladder tests cover instead.

## Assumptions and constraints

Shared by all books (see `order_book.hpp`):

- Prices are integer ticks, never floating point. Quantities are 32-bit.
- Trades execute at the resting order's price, best price first and FIFO
  within a level.
- An unfilled market-order remainder is discarded; it never rests.
- `modify` to the same price with an equal or smaller quantity keeps queue
  priority. Anything else is cancel then re-add: it loses priority and may
  trade.
- One instrument per book; single-threaded; no persistence. Trades are
  appended to a caller-owned vector so the hot path never allocates for them.

Specific to `FlatOrderBook` and `TunedOrderBook`:

- **Prices must be within a configured range.** The default is
  `[0, 1'000'000]` ticks, about 16 MB of levels per book. Limit orders and
  modifies outside the range are rejected. A rejected modify leaves the order
  where it was rather than cancelling it. The `OrderBook` concept doesn't
  list these rejection cases.
- **Order ids must be small and roughly dense,** because the index is a
  vector indexed by id. It grows by doubling, so ids don't need to be bounded
  up front, but one huge sparse id would allocate a huge table. Tuned also
  rejects limit-order ids at or above 2^32; market-order ids are not limited
  because they never rest.
- Huge pages are requested, not required. If the kernel declines, the book
  works on 4 KiB pages.

## Testing

- **Shared unit tests** (`tests/order_book_test.cpp`) run against every book,
  including the naive oracle itself.
- **Differential test** (`tests/differential_test.cpp`): 50 seeds × 10,000
  random operations, checking trades and all observable state against
  `NaiveOrderBook` after every step. Its narrow price band keeps the book
  dense, so it exercises partial fills, sweeps and emptied levels.
- **Ladder tests** (`tests/ladder_book_test.cpp`) cover what the shared tests
  can't:
  - the configured price range and its extreme prices;
  - id-index growth;
  - a sparse differential test over 300,000 ticks, which exercises the
    bitmap's cross-word search. A deliberately planted off-by-one in that
    search fails this test.
- The debug preset runs everything under ASan and UBSan.

## Future improvements

Matching and order features:

- **Self-trade prevention.** Orders need an owner (user or account id). The
  usual policies are to cancel the newest order, cancel the oldest, cancel
  both, or decrement both by the overlapping quantity. The check sits in the
  match loop, so the owner id should go in `Order` (Tuned has no spare bytes;
  one option is a parallel array indexed by pool slot).
- **Time in force and order types:**
  - IOC (fill what you can, cancel the rest);
  - FOK (all or nothing, which needs a check of available quantity before
    any fill);
  - GTC, Day and GTD (which need a clock and expiry processing);
  - post-only (reject if it would cross);
  - stop and stop-limit (a trigger book keyed by stop price);
  - iceberg (show a slice and replenish it, losing priority each time).
- **Richer rejection reporting.** Flat and Tuned have four reasons to reject an
  order (zero quantity, duplicate id, price out of range, id too large), but
  `add_limit` returns a `bool`. A C++23 `std::expected<void, Reject>` would
  say which one applied.

Instruments:

- **Multiple instruments.** A symbol → book map, with each book owning its
  own price range and tick size, and an order router in front. For
  throughput, shard books across cores with one thread per core and no
  shared state.
- **Options and other derivatives.** Each strike and expiry is its own
  instrument, so an option chain means hundreds of small, sparse books. That
  favours the bitmap or a `std::flat_map`-based book over large fixed
  ladders. Multi-leg orders (spreads, straddles) need atomic matching across
  books, implied pricing between legs and outrights, and per-instrument tick
  tables.
- **Unbounded price ranges.** A `std::flat_map` (C++23) book would keep
  storage contiguous without a fixed range. Store bids ascending so the best
  price is at `back()`, where erasing it is O(1).

Data and tooling:

- **Replay tool.** Stream an event log through a book and render the depth
  ladder, trades and spread over time, either as a terminal UI or by
  exporting snapshots to a web page. The benchmark's `Workload` already has a
  suitable event format.
- **Real exchange data.** The book matches individual orders, so it needs
  order-by-order (L3) data. Aggregated L2 depth can rebuild a depth view but
  can't drive matching. Public sources include NASDAQ TotalView-ITCH sample
  files, LOBSTER sample data, and crypto exchanges' full-order-book feeds. An
  ITCH parser that maps add, cancel, execute and replace messages onto this
  API would also give the benchmark a realistic workload with real
  sparseness and cancel patterns.

Performance and measurement:

- Run the books in alternating order in the benchmark, report the minimum of
  the repetitions alongside the median, and count samples above a threshold
  (for example 10 µs) so that host noise is visible rather than silently
  folded into max.
- Time individual events in cycles with `rdpmc`, avoiding the 10 ns TSC
  steps.
- Benchmark on bare-metal Linux with an isolated core (`isolcpus`,
  `nohz_full`, `rcu_nocbs`), the `performance` governor with boost off, and
  `SCHED_FIFO` plus `mlockall`.
- Keep a running total quantity per level, making `volume_at` O(1) (useful
  for L2 market-data output).
- Shrink Flat's 24-byte `Locator` to 16 bytes, or adopt Tuned's 8-byte
  layout, to cut index footprint without the rest of Tuned's changes.
- C++20 modules could hide implementation details such as
  `tuned_detail::HugePageAllocator`. The payoff is small for a header-only
  library, and CMake needs the Ninja generator for modules.

## Adding an implementation

1. Write a class satisfying `OrderBook` and add `static_assert(OrderBook<YourBook>);`.
2. Add it to `AllBooks` and `BooksUnderTest` in `tests/test_support.hpp`
   (and `LadderBooks` if it has a fixed price range and id-indexed orders).
3. Add a `run_book<YourBook>(...)` line in `bench/bench.cpp`. Its checksum
   should match `MapOrderBook`'s on the same workload.
