#include "taskforge/thread_pool.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace taskforge {
namespace {

TEST(ThreadPoolTest, ConstructsAndDestroysWithoutTasks) {
    ThreadPool pool(2);
    EXPECT_EQ(pool.worker_count(), 2U);
}

TEST(ThreadPoolTest, RejectsZeroWorkers) {
    EXPECT_THROW((ThreadPool(0)), std::invalid_argument);
}

TEST(ThreadPoolTest, RunsTasksWithOneAndMultipleWorkers) {
    for (const std::size_t worker_count : {1U, 4U}) {
        ThreadPool pool(worker_count);
        auto result = pool.submit([](int left, int right) { return left + right; }, 19, 23);
        EXPECT_EQ(result.get(), 42);
    }
}

TEST(ThreadPoolTest, SupportsVoidTasks) {
    ThreadPool pool(1);
    std::atomic<int> executions = 0;

    auto result = pool.submit([&executions] { ++executions; });
    result.get();

    EXPECT_EQ(executions.load(), 1);
}

TEST(ThreadPoolTest, SupportsMoveOnlyCallablesAndArguments) {
    ThreadPool pool(1);
    auto callable_state = std::make_unique<int>(10);
    auto argument = std::make_unique<int>(32);

    auto result = pool.submit(
        [state = std::move(callable_state)](std::unique_ptr<int> value) { return *state + *value; },
        std::move(argument));

    EXPECT_EQ(result.get(), 42);
    EXPECT_EQ(callable_state, nullptr);
    EXPECT_EQ(argument, nullptr);
}

TEST(ThreadPoolTest, SupportsReferenceWrapperArguments) {
    ThreadPool pool(1);
    int value = 20;

    pool.submit([](int &target) { target += 22; }, std::ref(value)).get();

    EXPECT_EQ(value, 42);
}

TEST(ThreadPoolTest, PropagatesTaskExceptionsThroughFuture) {
    ThreadPool pool(1);
    auto result = pool.submit([]() -> int { throw std::runtime_error("task failure"); });

    EXPECT_THROW((void)result.get(), std::runtime_error);
}

TEST(ThreadPoolTest, ExecutesEveryTaskExactlyOnceFromConcurrentProducers) {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t tasks_per_producer = 64;
    constexpr std::size_t task_count = producer_count * tasks_per_producer;
    ThreadPool pool(4);
    auto executions = std::make_unique<std::atomic<int>[]>(task_count);
    std::barrier start(static_cast<std::ptrdiff_t>(producer_count));
    std::vector<std::thread> producers;
    std::vector<std::future<void>> results;
    std::mutex results_mutex;

    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            start.arrive_and_wait();
            for (std::size_t task = 0; task < tasks_per_producer; ++task) {
                const auto index = producer * tasks_per_producer + task;
                auto result = pool.submit([&, index] { ++executions[index]; });
                std::lock_guard lock(results_mutex);
                results.push_back(std::move(result));
            }
        });
    }
    for (auto &producer : producers) {
        producer.join();
    }

    ASSERT_EQ(results.size(), task_count);
    for (auto &result : results) {
        result.get();
    }
    for (std::size_t index = 0; index < task_count; ++index) {
        EXPECT_EQ(executions[index].load(), 1) << "task index " << index;
    }
}

TEST(ThreadPoolTest, ShutdownDrainsAcceptedQueuedAndRunningTasks) {
    ThreadPool pool(1);
    std::latch task_started(1);
    std::latch release_task(1);
    auto running = pool.submit([&] {
        task_started.count_down();
        release_task.wait();
    });
    task_started.wait();

    std::atomic<int> queued_executions = 0;
    auto queued = pool.submit([&] { ++queued_executions; });
    std::thread shutdown_thread([&] { pool.shutdown(); });
    release_task.count_down();
    shutdown_thread.join();

    running.get();
    queued.get();
    EXPECT_EQ(queued_executions.load(), 1);
}

TEST(ThreadPoolTest, RepeatedShutdownIsSafeAndRejectsNewTasks) {
    ThreadPool pool(2);
    pool.shutdown();
    pool.shutdown();

    EXPECT_THROW(pool.submit([] {}), std::runtime_error);
}

TEST(ThreadPoolTest, RejectsShutdownFromOneOfItsWorkers) {
    ThreadPool pool(1);
    auto result = pool.submit([&pool] { pool.shutdown(); });

    EXPECT_THROW(result.get(), std::logic_error);
    pool.shutdown();
}

TEST(ThreadPoolTest, SubmissionRacingWithShutdownIsCompletedOrRejected) {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t attempts_per_producer = 100;
    ThreadPool pool(3);
    std::barrier start(static_cast<std::ptrdiff_t>(producer_count + 1));
    std::atomic<std::size_t> executions = 0;
    std::mutex results_mutex;
    std::vector<std::future<void>> accepted;
    std::vector<std::thread> producers;

    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&] {
            start.arrive_and_wait();
            for (std::size_t attempt = 0; attempt < attempts_per_producer; ++attempt) {
                try {
                    auto result = pool.submit([&] { ++executions; });
                    std::lock_guard lock(results_mutex);
                    accepted.push_back(std::move(result));
                } catch (const std::runtime_error &) {
                    break;
                }
            }
        });
    }

    start.arrive_and_wait();
    pool.shutdown();
    for (auto &producer : producers) {
        producer.join();
    }

    for (auto &result : accepted) {
        result.get();
    }
    EXPECT_EQ(executions.load(), accepted.size());
}

} // namespace
} // namespace taskforge
