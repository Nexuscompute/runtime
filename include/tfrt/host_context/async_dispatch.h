/*
 * Copyright 2020 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This file declares functions related to asynchronous work dispatching.

#ifndef TFRT_HOST_CONTEXT_ASYNC_DISPATCH_H_
#define TFRT_HOST_CONTEXT_ASYNC_DISPATCH_H_

#include "tfrt/host_context/execution_context.h"
#include "tfrt/host_context/host_context.h"
#include "tfrt/support/latch.h"

namespace tfrt {

namespace internal {
// Extract result type for EnqueueWork and EnqueueBlockingWork.
template <typename T>
struct UnwrapExpected {
  using type = T;
};
template <typename T>
struct UnwrapExpected<Expected<T>> {
  using type = T;
};

template <typename F>
using AsyncResultTypeT = typename UnwrapExpected<std::result_of_t<F()>>::type;

}  // namespace internal

// AndThen functions more friendly for generic programming.
template <typename F>
void AndThen(AsyncValue* value, F&& f) {
  value->AndThen(std::forward<F>(f));
}

template <typename F>
void AndThen(const RCReference<AsyncValue>& value, F&& f) {
  value->AndThen(std::forward<F>(f));
}

template <typename T, typename F>
void AndThen(const AsyncValueRef<T>& value, F&& f) {
  value.AndThen(std::forward<F>(f));
}

// Block until the specified values are available (either with a value or an
// error result).
// `values` can be any range of AsyncValues.
// Example usages:
//
// std::vector<AsycnValueRef<int>> av_refs;
// ...
// AwaitRange(av_refs);
//
// // Foo is a struct contains an AsyncValue.
// std::vector<Foo> foos;
// AwaitRange(llvm::map_range(foos, [](auto& foo) { return foo.async_value; }));
template <typename RangeT>
void AwaitRange(const RangeT& values) {
  // We are done when values_remaining drops to zero.
  tfrt::latch values_remaining(std::distance(values.begin(), values.end()));

  // As each value becomes available, we decrement the count.
  for (const auto& value : values) {
    AndThen(value, [&values_remaining] { values_remaining.count_down(); });
  }

  // Wait until all values are resolved.
  values_remaining.wait();
}

inline void Await(ArrayRef<AsyncValue*> values) { AwaitRange(values); }

inline void Await(ArrayRef<RCReference<AsyncValue>> rc_refs) {
  AwaitRange(rc_refs);
}

// Block until the specified values are available (either with a value or an
// error result). It uses the work queue inside `exec_ctx`, so, depending on the
// queue implementation, this should not be called by a thread managed by the
// work queue.
void Await(const ExecutionContext& exec_ctx,
           ArrayRef<RCReference<AsyncValue>> values);

// Add some non-blocking work to the work_queue used by the ExecutionContext.
void EnqueueWork(const ExecutionContext& exec_ctx,
                 llvm::unique_function<void()> work);

// Overload of EnqueueWork that return AsyncValueRef<R> for work that returns R
// when R is not void.
//
// Example:
// int a = 1, b = 2;
// AsyncValueRef<int> r = EnqueueWork(exec_ctx, [a, b] { return a + b; });
template <typename F, typename R = internal::AsyncResultTypeT<F>,
          std::enable_if_t<!std::is_void<R>(), int> = 0>
[[nodiscard]] AsyncValueRef<R> EnqueueWork(const ExecutionContext& exec_ctx,
                                           F&& work) {
  auto result = MakeUnconstructedAsyncValueRef<R>(exec_ctx.host());
  EnqueueWork(exec_ctx, [result = result.CopyRef(),
                         work = std::forward<F>(work)]() mutable {
    result.emplace(work());
  });
  return result;
}

// The following set of functions scheduled a blocking or non-blocking work
// without an ExecutionContext. They should only be used for tasks that are
// outside of a kernel execution. Depending on the thread pool implementation,
// such tasks are typically scheduled at the default priority.
void EnqueueWork(HostContext* host, llvm::unique_function<void()> work);

// Overload of EnqueueWork that return AsyncValueRef<R> for work that returns R
// when R is not void.
//
// Example:
// int a = 1, b = 2;
// AsyncValueRef<int> r = EnqueueWork(host_ctx, [a, b] { return a + b; });
template <typename F, typename R = internal::AsyncResultTypeT<F>,
          std::enable_if_t<!std::is_void<R>(), int> = 0>
[[nodiscard]] AsyncValueRef<R> EnqueueWork(HostContext* host, F&& work) {
  auto result = MakeUnconstructedAsyncValueRef<R>(host);
  EnqueueWork(host, [result = result.CopyRef(),
                     work = std::forward<F>(work)]() mutable {
    result.emplace(work());
  });
  return result;
}

[[nodiscard]] bool EnqueueBlockingWork(HostContext* host,
                                       llvm::unique_function<void()> work);

// Overload of EnqueueBlockingWork that return AsyncValueRef<R> for work that
// returns R when R is not void.
//
// Example:
// int a = 1, b = 2;
// AsyncValueRef<int> r = EnqueueBlockingWork(host_ctx, [a, b] { return a + b;
// });
template <typename F, typename R = internal::AsyncResultTypeT<F>,
          std::enable_if_t<!std::is_void<R>(), int> = 0>
[[nodiscard]] AsyncValueRef<R> EnqueueBlockingWork(HostContext* host,
                                                   F&& work) {
  auto result = MakeUnconstructedAsyncValueRef<R>(host);
  bool enqueued = EnqueueBlockingWork(
      host,
      [result = result.CopyRef(), work = std::forward<F>(work)]() mutable {
        result.emplace(work());
      });
  if (!enqueued) {
    result.SetError("Failed to enqueue blocking work.");
  }
  return result;
}

[[nodiscard]] bool RunBlockingWork(HostContext* host,
                                   llvm::unique_function<void()> work);

// Overload of RunBlockingWork that return AsyncValueRef<R> for work that
// returns R when R is not void.
//
// Example:
// int a = 1, b = 2;
// AsyncValueRef<int> r = RunBlockingWork(host_ctx, [a, b] { return a + b;
// });
template <typename F, typename R = internal::AsyncResultTypeT<F>,
          std::enable_if_t<!std::is_void<R>(), int> = 0>
[[nodiscard]] AsyncValueRef<R> RunBlockingWork(HostContext* host, F&& work) {
  auto result = MakeUnconstructedAsyncValueRef<R>(host);
  bool enqueued = RunBlockingWork(
      host,
      [result = result.CopyRef(), work = std::forward<F>(work)]() mutable {
        result.emplace(work());
      });
  if (!enqueued) {
    result.SetError("Failed to run blocking work.");
  }
  return result;
}

// Run the specified function when the specified set of AsyncValue's are all
// resolved.  This is a set-version of "AndThen".
void RunWhenReady(ArrayRef<AsyncValue*> values,
                  llvm::unique_function<void()> callee);

void RunWhenReady(ArrayRef<RCReference<AsyncValue>> values,
                  llvm::unique_function<void()> callee);

void Await(HostContext* host, ArrayRef<RCReference<AsyncValue>> values);

template <typename T>
void Await(HostContext* host, const AsyncValueRef<T>& av_ref) {
  // It is unfornate that we need to do a CopyRef() here. The root cause of this
  // is that ConcurrentWorkQueue::Await() takes
  // ArrayRef<RCReference<AsyncValue>> which requires the client to put
  // RCReference<AsyncValue> in contiguous memory space.
  //
  // TODO(jingdong): Fix this by providing an overload of
  // ConcurrentWorkQueue::Await() that takes ArrayRef<AsyncValue*>.
  Await(host, {av_ref.CopyRef()});
}

}  // namespace tfrt

#endif  // TFRT_HOST_CONTEXT_ASYNC_DISPATCH_H_
