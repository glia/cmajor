//
//     ,ad888ba,                              88
//    d8"'    "8b
//   d8            88,dba,,adba,   ,aPP8A.A8  88     The Cmajor Toolkit
//   Y8,           88    88    88  88     88  88
//    Y8a.   .a8P  88    88    88  88,   ,88  88     (C)2024 Cmajor Software Ltd
//     '"Y888Y"'   88    88    88  '"8bbP"Y8  88     https://cmajor.dev
//                                           ,88
//                                        888P"
//
//  The Cmajor project is subject to commercial or open-source licensing.
//  You may use it under the terms of the GPLv3 (see www.gnu.org/licenses), or
//  visit https://cmajor.dev to learn about our commercial licence options.
//
//  CMAJOR IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
//  EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
//  DISCLAIMED.

#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include <functional>
#include <cstdint>

#ifdef __APPLE__
 #include <dispatch/dispatch.h>
 #include <pthread.h>
#endif

namespace cmaj
{

//==============================================================================
/// A real-time-safe parallel work group for dispatching independent work items
/// across multiple CPU cores. Designed for audio processing where:
///   - Thread count is fixed at construction time
///   - No dynamic allocation occurs during dispatch
///   - Spin-wait barriers avoid kernel-level synchronization overhead
///   - On Apple Silicon, uses GCD for optimal core assignment
///
/// Usage:
///   ParallelWorkGroup workGroup (4);  // 4 worker threads
///   workGroup.parallelFor (88, [&] (uint32_t index) {
///       processNeuron (index);
///   });
///
struct ParallelWorkGroup
{
    /// Creates a work group with the specified number of worker threads.
    /// If numThreads is 0, it defaults to the number of performance cores
    /// on the system (capped at 8 for audio workloads).
    explicit ParallelWorkGroup (uint32_t numThreads = 0)
    {
        if (numThreads == 0)
            numThreads = getRecommendedThreadCount();

        threadCount = std::max (1u, numThreads);

       #ifdef __APPLE__
        // Use GCD concurrent queue for optimal Apple Silicon scheduling.
        // QOS_CLASS_USER_INTERACTIVE maps to performance cores on M1.
        auto qosAttr = dispatch_queue_attr_make_with_qos_class (
            DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INTERACTIVE, 0);
        dispatchQueue = dispatch_queue_create ("com.cmajor.parallel", qosAttr);
       #else
        // On other platforms, create a pool of worker threads that spin-wait
        // for work items. This avoids kernel scheduling overhead.
        workers.resize (threadCount - 1);

        for (uint32_t i = 0; i < threadCount - 1; ++i)
        {
            workers[i].thread = std::thread ([this, i]
            {
                workerLoop (i);
            });

            setThreadPriority (workers[i].thread, true);
        }
       #endif
    }

    ~ParallelWorkGroup()
    {
       #ifdef __APPLE__
        if (dispatchQueue != nullptr)
            dispatch_release (dispatchQueue);
       #else
        shouldExit.store (true, std::memory_order_release);

        for (auto& w : workers)
        {
            w.hasWork.store (true, std::memory_order_release);

            if (w.thread.joinable())
                w.thread.join();
        }
       #endif
    }

    /// Dispatches `count` work items across all threads, calling `fn(index)`
    /// for each item. Blocks until all items are complete.
    /// This is safe to call from the audio thread.
    template <typename Fn>
    void parallelFor (uint32_t count, Fn&& fn)
    {
        if (count == 0)
            return;

        if (count <= threadCount || threadCount <= 1)
        {
            // Not worth parallelising - run sequentially
            for (uint32_t i = 0; i < count; ++i)
                fn (i);

            return;
        }

       #ifdef __APPLE__
        dispatchParallelFor_GCD (count, std::forward<Fn> (fn));
       #else
        dispatchParallelFor_ThreadPool (count, std::forward<Fn> (fn));
       #endif
    }

    /// Returns the number of threads in the work group.
    uint32_t getThreadCount() const     { return threadCount; }

    /// Returns the recommended thread count for audio workloads on this system.
    static uint32_t getRecommendedThreadCount()
    {
        auto cores = std::thread::hardware_concurrency();

       #ifdef __APPLE__
        // On Apple Silicon, use performance cores only (typically 4 on M1).
        // Efficiency cores have much lower single-thread performance and
        // would increase latency variance.
        cores = std::min (cores, 4u);
       #endif

        // Cap at 8 for audio - more threads means more synchronization cost
        return std::max (1u, std::min (cores, 8u));
    }

private:
    uint32_t threadCount = 1;

   #ifdef __APPLE__
    dispatch_queue_t dispatchQueue = nullptr;

    template <typename Fn>
    void dispatchParallelFor_GCD (uint32_t count, Fn&& fn)
    {
        // dispatch_apply partitions work across GCD threads and blocks until
        // all iterations complete. On Apple Silicon this will use performance
        // cores due to QOS_CLASS_USER_INTERACTIVE.
        __block auto fnRef = std::forward<Fn> (fn);

        dispatch_apply (static_cast<size_t> (count), dispatchQueue,
                        ^(size_t index) { fnRef (static_cast<uint32_t> (index)); });
    }

   #else
    struct alignas (64) Worker
    {
        std::thread thread;
        std::atomic<bool> hasWork { false };
        std::atomic<bool> isComplete { false };
        uint32_t startIndex = 0;
        uint32_t endIndex = 0;
        std::function<void(uint32_t)> workFn;
    };

    std::vector<Worker> workers;
    std::atomic<bool> shouldExit { false };

    void workerLoop (uint32_t workerIndex)
    {
        auto& self = workers[workerIndex];

        while (! shouldExit.load (std::memory_order_acquire))
        {
            // Spin-wait for work. In audio contexts this minimises wake-up
            // latency compared to condition variables.
            while (! self.hasWork.load (std::memory_order_acquire))
            {
                if (shouldExit.load (std::memory_order_relaxed))
                    return;

                // Brief pause to reduce power consumption while spinning
               #if defined(__arm__) || defined(__aarch64__)
                __asm__ volatile ("yield" ::: "memory");
               #elif defined(__x86_64__) || defined(__i386__)
                __asm__ volatile ("pause" ::: "memory");
               #else
                std::this_thread::yield();
               #endif
            }

            // Execute the assigned work partition
            for (uint32_t i = self.startIndex; i < self.endIndex; ++i)
                self.workFn (i);

            self.isComplete.store (true, std::memory_order_release);
            self.hasWork.store (false, std::memory_order_release);
        }
    }

    template <typename Fn>
    void dispatchParallelFor_ThreadPool (uint32_t count, Fn&& fn)
    {
        auto itemsPerThread = count / threadCount;
        auto remainder = count % threadCount;
        uint32_t offset = 0;

        // Assign work to each worker thread
        for (uint32_t i = 0; i < threadCount - 1; ++i)
        {
            auto items = itemsPerThread + (i < remainder ? 1 : 0);
            workers[i].startIndex = offset;
            workers[i].endIndex = offset + items;
            workers[i].workFn = fn;
            workers[i].isComplete.store (false, std::memory_order_release);
            workers[i].hasWork.store (true, std::memory_order_release);
            offset += items;
        }

        // Current thread handles the remaining work
        for (uint32_t i = offset; i < count; ++i)
            fn (i);

        // Spin-wait for all workers to complete
        for (uint32_t i = 0; i < threadCount - 1; ++i)
        {
            while (! workers[i].isComplete.load (std::memory_order_acquire))
            {
               #if defined(__arm__) || defined(__aarch64__)
                __asm__ volatile ("yield" ::: "memory");
               #elif defined(__x86_64__) || defined(__i386__)
                __asm__ volatile ("pause" ::: "memory");
               #endif
            }
        }
    }

    static void setThreadPriority (std::thread& t, bool highPriority)
    {
       #ifdef __APPLE__
        if (highPriority)
        {
            pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
        }
       #else
        (void) t;
        (void) highPriority;
       #endif
    }
   #endif

    // Non-copyable, non-moveable
    ParallelWorkGroup (const ParallelWorkGroup&) = delete;
    ParallelWorkGroup& operator= (const ParallelWorkGroup&) = delete;
};

} // namespace cmaj
