// Copyright (c) the JPEG XL Project Authors.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef TOOLS_THREAD_POOL_INTERNAL_H_
#define TOOLS_THREAD_POOL_INTERNAL_H_

#include <cstddef>
#include <memory>
#include <thread>  // NOLINT

#include "lib/base/common.h"
#include "lib/base/data_parallel.h"
#include "lib/threads/thread_parallel_runner.h"
#include "lib/threads/thread_parallel_runner_cxx.h"

namespace jpegxl {
namespace tools {

using ::jxl::ThreadPool;

// Helper class to pass an internal ThreadPool-like object using threads.
class ThreadPoolInternal {
 public:
  // Starts the given number of worker threads and blocks until they are ready.
  // "num_worker_threads" defaults to one per hyperthread. If zero, all tasks
  // run on the main thread.
  explicit ThreadPoolInternal(
      size_t num_threads = std::thread::hardware_concurrency()) {
    runner_ =
        JxlThreadParallelRunnerMake(/* memory_manager */ nullptr, num_threads);
    pool_ =
        jxl::make_unique<ThreadPool>(JxlThreadParallelRunner, runner_.get());
  }

  ThreadPoolInternal(const ThreadPoolInternal&) = delete;
  ThreadPoolInternal& operator&(const ThreadPoolInternal&) = delete;
  ThreadPool* get() { return pool_.get(); }

 private:
  JxlThreadParallelRunnerPtr runner_;
  std::unique_ptr<ThreadPool> pool_;
};

}  // namespace tools
}  // namespace jpegxl

#endif  // TOOLS_THREAD_POOL_INTERNAL_H_
