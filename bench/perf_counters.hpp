#pragma once

// Hardware performance counters (cycles, branch and cache misses) read through
// Linux perf_event_open. Counting mode only: the PMU increments the counters
// in hardware, and the program touches them just twice per measured region
// (enable before, disable + read after), so nothing is added inside the loop.
//
// Counters are user-space only (exclude_kernel), which works at the default
// perf_event_paranoid level of 2. On other platforms, or when the kernel or
// hypervisor exposes no PMU, `available()` is false and the bench skips them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

enum class Counter : std::size_t {
  Cycles,
  Instructions,
  Branches,
  BranchMisses,
  L1dMisses,
  CacheMisses,
};
inline constexpr std::size_t kCounters = 6;

// One reading per counter, scaled up if the kernel had to multiplex.
// `multiplexed` is the smallest fraction of the region a counter was live;
// below 1.0 the values are estimates.
struct CounterValues {
  std::array<double, kCounters> value{};
  std::array<bool, kCounters> valid{};
  double multiplexed = 1.0;

  double operator[](Counter c) const {
    return value[static_cast<std::size_t>(c)];
  }
  bool has(Counter c) const { return valid[static_cast<std::size_t>(c)]; }
};

#if defined(__linux__)

// Encodes a PERF_TYPE_HW_CACHE config: which cache, which op, hit or miss.
constexpr std::uint64_t perf_hw_cache(std::uint64_t cache, std::uint64_t op,
                                      std::uint64_t result) {
  return cache | (op << 8) | (result << 16);
}

class PerfCounters {
public:
  PerfCounters() {
    for (std::size_t i = 0; i < kCounters; ++i)
      fds_[i] = open_event(kEvents[i].type, kEvents[i].config);
  }

  ~PerfCounters() {
    for (int fd : fds_)
      if (fd >= 0)
        close(fd);
  }

  PerfCounters(const PerfCounters &) = delete;
  PerfCounters &operator=(const PerfCounters &) = delete;

  bool available() const {
    for (int fd : fds_)
      if (fd >= 0)
        return true;
    return false;
  }

  void start() {
    for (int fd : fds_)
      if (fd >= 0)
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    for (int fd : fds_)
      if (fd >= 0)
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
  }

  CounterValues stop() {
    for (int fd : fds_)
      if (fd >= 0)
        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

    CounterValues out;
    for (std::size_t i = 0; i < kCounters; ++i) {
      if (fds_[i] < 0)
        continue;
      std::uint64_t buf[3]; // value, time_enabled, time_running
      if (read(fds_[i], buf, sizeof buf) != static_cast<ssize_t>(sizeof buf) ||
          buf[2] == 0)
        continue;
      const double live =
          static_cast<double>(buf[2]) / static_cast<double>(buf[1]);
      out.value[i] = static_cast<double>(buf[0]) / live;
      out.valid[i] = true;
      if (live < out.multiplexed)
        out.multiplexed = live;
    }
    return out;
  }

private:
  struct EventSpec {
    std::uint32_t type;
    std::uint64_t config;
  };

  // Same order as `Counter`.
  static constexpr std::array<EventSpec, kCounters> kEvents = {{
      {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
      {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
      {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
      {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
      {PERF_TYPE_HW_CACHE,
       perf_hw_cache(PERF_COUNT_HW_CACHE_L1D, PERF_COUNT_HW_CACHE_OP_READ,
                     PERF_COUNT_HW_CACHE_RESULT_MISS)},
      {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
  }};

  static int open_event(std::uint32_t type, std::uint64_t config) {
    perf_event_attr attr;
    std::memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.type = type;
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format =
        PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
  }

  std::array<int, kCounters> fds_{};
};

#else

class PerfCounters {
public:
  bool available() const { return false; }
  void start() {}
  CounterValues stop() { return {}; }
};

#endif
