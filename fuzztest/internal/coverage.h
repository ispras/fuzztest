// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Coverage interface.
//
// We rely on SanitizerCoverage instrumentation for coverage feedback:
// https://clang.llvm.org/docs/SanitizerCoverage.html
//
// Currently, we use the inline counters feature of SanCov. To enable the
// instrumentation, we need to compile with:
//
//   -fsanitize-coverage=inline-8bit-counters
//
// This will create a 8-bit counter for each edge in the code.

#ifndef FUZZTEST_FUZZTEST_INTERNAL_COVERAGE_H_
#define FUZZTEST_FUZZTEST_INTERNAL_COVERAGE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "absl/types/span.h"
#include "./fuzztest/internal/table_of_recent_compares.h"

namespace fuzztest::internal {

// Represents the coverage information generated by the SanitizerCoverage
// instrumentation. Used for storing the coverage of a single input's execution.
//
// The counters are non-atomic. Race conditions are ignored. As well as
// overflows. Single threaded processes are more ideal for tests.
class ExecutionCoverage {
 public:
  explicit ExecutionCoverage(absl::Span<uint8_t> counter_map)
      : counter_map_(counter_map) {}

  // not copyable or movable because sanCov will only see one global instance.
  ExecutionCoverage(const ExecutionCoverage&) = delete;
  ExecutionCoverage& operator=(const ExecutionCoverage&) = delete;

  // Returns a view to the counter map.
  absl::Span<uint8_t> GetCounterMap() const { return counter_map_; }

  // Clears the counter map state and cmp/memcmp coverage states.
  // Not clearing tables_of_recent_compares_, this might introduce some
  // false positives in the dictionary, but the probability is low
  // from theory and experiments.
  void ResetState() {
    memset(new_cmp_counter_map_, 0, kCmpCovMapSize);
    memset(counter_map_.data(), 0, counter_map_.size());
    new_coverage_.store(false, std::memory_order_relaxed);

    max_stack_recorded_ = 0;
    test_thread_stack_top = GetCurrentStackFrame();
  }

  static char* GetCurrentStackFrame() {
#if defined(__has_builtin) && __has_builtin(__builtin_frame_address)
    return reinterpret_cast<char*>(__builtin_frame_address(0));
#else
    return nullptr;
#endif
  }

  // The size of cmp coverage maps, it's a randomly picked value. But
  // in the past we observe that it shouldn't be > 1MB otherwise it will
  // be the bottleneck. Now it's 256KB.
  static constexpr size_t kCmpCovMapSize = 1024U * 256;
  // Try update comparison coverage score.
  // If a higher score is found, set new_cmp_ to be mark a new coverage.
  void UpdateCmpMap(size_t index, uint8_t hamming_dist, uint8_t absolute_dist);

  bool NewCoverageFound() {
    return new_coverage_.load(std::memory_order_relaxed);
  }

  auto& GetTablesOfRecentCompares() { return tables_of_recent_compares_; }

  // Flag marking if the control flow is currently in target codes.
  // We don't want to collect unrelated updates to cmp score and dictionary.
  bool IsTracing() { return is_tracing_; }

  // Right before target run, call this method with true; right after
  // target run, call this method with false.
  void SetIsTracing(bool is_tracing) { is_tracing_ = is_tracing; }

  // Most of the time tests are run under the main thread, which has a very
  // large stack limit. On the other hand, code under test will tend to run on
  // threads with a much smaller stack size.
  // Instead of waiting for a stack overflow, we measure the stack usage and
  // abort the process if it finds it to be larger than
  // `MaxAllowedStackUsage()`. This limit can be configured via environment
  // variable `FUZZTEST_STACK_LIMIT`.
  size_t MaxAllowedStackUsage();
  // Update the PC->max stack usage map for `PC`.
  // It compares the current stack frame against the stack frame specified in
  // `test_thread_stack_top`. Only applies to the thread that sets
  // `test_thread_stack_top` and it is a noop on other threads.
  void UpdateMaxStack(uintptr_t PC);
  size_t MaxStackUsed() const { return max_stack_recorded_; }

 private:
  // 8-bit counters map, records the hit of edges.
  absl::Span<uint8_t> counter_map_;

  // New cmp coverage map.
  // `counter`: max counts of hits of a cmp instruction.
  // `hamming`: `sizeof(arg)` - hamming distance between the two args of a cmp
  // instruction.
  // `absolute`: 255 - `min(255, abs(arg1 - arg2))`. Absolute distance score.
  // If `counter_new` > `counter_old`, the score increases.
  // Else if `counter_new` == `counter_old`, then compare `hamming` and
  // `absolute`, any one of them has new value larger than old value, the score
  // increases.
  struct CmpScore {
    uint8_t counter;
    uint8_t hamming;
    uint8_t absolute;
  };
  uint8_t new_cmp_counter_map_[kCmpCovMapSize] = {};

  // Temporary map storing the hit counts of cmps for a target run.
  CmpScore max_cmp_map_[kCmpCovMapSize] = {};

  static inline thread_local const char* test_thread_stack_top = nullptr;
  // The watermark of stack usage observed on the test thread while tracing is
  // enabled.
  ptrdiff_t max_stack_recorded_ = 0;

  // Like on other coverage maps above, we record the max stack usage on
  // different PC seen. This allows the runtime to mark "more stack usage" as
  // "new coverage" per PC instead of globally.
  static constexpr size_t kMaxStackMapSize = 1024U * 256;
  uint32_t max_stack_map_[kMaxStackMapSize] = {};

  // Flag marking new coverage of any kind.
  std::atomic<bool> new_coverage_{false};

  TablesOfRecentCompares tables_of_recent_compares_ = {};
  bool is_tracing_ = false;
};

// Returns the singleton ExecutionCoverage object.
ExecutionCoverage* GetExecutionCoverage();

// Represents the aggregate coverage of all inputs in the corpus. Used for
// detecting if new coverage was triggered by executing an input.
class CorpusCoverage {
 public:
  // Creates initial blank coverage state with `map_size` counters. The map
  // contains one counter per edge.
  explicit CorpusCoverage(size_t map_size);
  ~CorpusCoverage();

  CorpusCoverage(const CorpusCoverage&) = delete;
  CorpusCoverage& operator=(const CorpusCoverage&) = delete;

  // Merges `execution_coverage` into corpus coverage. Returns true if new
  // coverage was triggered.
  bool Update(ExecutionCoverage* execution_coverage);

  // Returns the number of unique edges covered by the corpus.
  size_t GetNumberOfCoveredEdges() const {
    const size_t uncovered_edges =
        std::count(corpus_map_, corpus_map_ + corpus_map_size_, 0);
    return corpus_map_size_ - uncovered_edges;
  }

 private:
  size_t corpus_map_size_;
  uint8_t* corpus_map_;
};

}  // namespace fuzztest::internal

#endif  // FUZZTEST_FUZZTEST_INTERNAL_COVERAGE_H_
