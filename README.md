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
| `tests/naive_order_book.hpp` | Linear-scan reference book used as the test oracle |
| `tests/order_book_test.cpp` | Unit tests, run against every implementation |
| `tests/differential_test.cpp` | Random operations compared step by step against the reference book |
| `bench/workload.hpp` | Deterministic synthetic order flow generator |
| `bench/bench.cpp` | Throughput, per-operation latency percentiles, and hardware counters |
| `bench/perf_counters.hpp` | `perf_event_open` wrapper for cycle, branch-miss and cache-miss counts |

## Adding an implementation

1. Write a class satisfying `OrderBook` and add `static_assert(OrderBook<YourBook>);`.
2. Add it to `AllBooks` and `BooksUnderTest` in `tests/test_support.hpp`.
3. Add a `run_book<YourBook>(...)` line in `bench/bench.cpp`. Its checksum
   should match `MapOrderBook`'s on the same workload.
