#include "taskforge/thread_pool.hpp"
#include "taskforge/work_stealing_pool.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace taskforge {
namespace {

std::uint64_t bounded_work(std::uint32_t iterations) {
    std::uint64_t value = 1;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        value = value * 6364136223846793005ULL + 1U + index;
    }
    return value;
}

TEST(WorkStealingThreadPoolTest, ConstructsAndDestroysWithoutTasks) {
    WorkStealingThreadPool pool(2);
    EXPECT_EQ(pool.worker_count(), 2U);
}

TEST(WorkStealingThreadPoolTest, RejectsZeroWorkers) {
    EXPECT_THROW((WorkStealingThreadPool(0)), std::invalid_argument);
}

TEST(WorkStealingThreadPoolTest, RunsExternalSubmissions) {
    for (const std::size_t worker_count : {1U, 3U}) {
        WorkStealingThreadPool pool(worker_count);
        auto result = pool.submit([](int left, int right) { return left + right; }, 19, 23);
        EXPECT_EQ(result.get(), 42);
    }
}

TEST(WorkStealingThreadPoolTest, SupportsMoveOnlyCallablesArgumentsAndVoidTasks) {
    WorkStealingThreadPool pool(2);
    auto callable_value = std::make_unique<int>(10);
    auto argument = std::make_unique<int>(32);
    auto result = pool.submit(
        [owned = std::move(callable_value)](std::unique_ptr<int> value) { return *owned + *value; },
        std::move(argument));
    EXPECT_EQ(result.get(), 42);
    EXPECT_EQ(callable_value, nullptr);
    EXPECT_EQ(argument, nullptr);

    std::atomic<int> executions = 0;
    pool.submit([&] { ++executions; }).get();
    EXPECT_EQ(executions.load(), 1);
}

TEST(WorkStealingThreadPoolTest, PropagatesTaskExceptionsThroughFuture) {
    WorkStealingThreadPool pool(2);

    auto failure = pool.submit([]() -> int { throw std::runtime_error("task failure"); });
    EXPECT_THROW((void)failure.get(), std::runtime_error);
}

TEST(WorkStealingThreadPoolTest, LocalTasksAreStolenByAnotherWorker) {
    WorkStealingThreadPool pool(2);
    std::latch parent_started(1);
    std::latch child_stolen(1);
    std::latch release_parent(1);
    std::atomic<int> child_executions = 0;
    std::atomic<int> stolen_executions = 0;
    std::atomic<bool> reported_steal = false;

    auto parent = pool.submit([&] {
        const auto owner = std::this_thread::get_id();
        parent_started.count_down();
        for (int index = 0; index < 64; ++index) {
            (void)pool.submit([&, owner] {
                ++child_executions;
                if (std::this_thread::get_id() != owner) {
                    ++stolen_executions;
                    if (!reported_steal.exchange(true)) {
                        child_stolen.count_down();
                    }
                }
            });
        }
        release_parent.wait();
    });

    parent_started.wait();
    child_stolen.wait();
    release_parent.count_down();
    parent.get();
    pool.shutdown();

    EXPECT_EQ(child_executions.load(), 64);
    EXPECT_GE(stolen_executions.load(), 1);
}

TEST(WorkStealingThreadPoolTest, ShutdownDrainsAcceptedTasksAndIsRepeatable) {
    WorkStealingThreadPool pool(3);
    std::atomic<int> executions = 0;
    constexpr int task_count = 200;
    for (int index = 0; index < task_count; ++index) {
        (void)pool.submit([&] { ++executions; });
    }

    pool.shutdown();
    pool.shutdown();
    EXPECT_EQ(executions.load(), task_count);
    EXPECT_THROW(pool.submit([] {}), std::runtime_error);
}

TEST(WorkStealingThreadPoolTest, ShutdownDrainsRunningAndQueuedTasks) {
    WorkStealingThreadPool pool(1);
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

TEST(WorkStealingThreadPoolTest, RejectsShutdownFromOneOfItsWorkers) {
    WorkStealingThreadPool pool(1);
    auto result = pool.submit([&pool] { pool.shutdown(); });

    EXPECT_THROW(result.get(), std::logic_error);
    pool.shutdown();
}

TEST(WorkStealingThreadPoolTest, PoolsRemainIndependent) {
    ThreadPool baseline(1);
    WorkStealingThreadPool work_stealing(1);

    auto baseline_result = baseline.submit([] { return 11; });
    baseline.shutdown();
    EXPECT_EQ(baseline_result.get(), 11);

    auto work_stealing_result = work_stealing.submit([] { return 31; });
    EXPECT_EQ(work_stealing_result.get(), 31);
    work_stealing.shutdown();
}

TEST(WorkStealingThreadPoolTest, SubmissionRacingWithShutdownIsCompletedOrRejected) {
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t attempts_per_producer = 100;
    WorkStealingThreadPool pool(3);
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

TEST(WorkStealingThreadPoolTest, BoundedSeededSubmissionStress) {
    constexpr std::uint32_t seed = 0x5EED1234U;
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t tasks_per_producer = 96;
    RecordProperty("random_seed", "0x5EED1234");
    SCOPED_TRACE("bounded stress random seed: 0x5EED1234");
    std::cout << "Bounded work-stealing stress random seed: 0x5EED1234\n";

    WorkStealingThreadPool pool(4);
    std::barrier start(static_cast<std::ptrdiff_t>(producer_count));
    std::mutex results_mutex;
    std::vector<std::pair<std::future<std::uint64_t>, std::uint64_t>> results;
    std::vector<std::thread> producers;

    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            std::mt19937 generator(seed + static_cast<std::uint32_t>(producer));
            std::uniform_int_distribution<std::uint32_t> workload(1, 800);
            start.arrive_and_wait();
            for (std::size_t task = 0; task < tasks_per_producer; ++task) {
                const auto iterations = workload(generator);
                const auto expected = bounded_work(iterations);
                auto result = pool.submit([iterations] { return bounded_work(iterations); });
                std::lock_guard lock(results_mutex);
                results.emplace_back(std::move(result), expected);
            }
        });
    }
    for (auto &producer : producers) {
        producer.join();
    }

    pool.shutdown();
    ASSERT_EQ(results.size(), producer_count * tasks_per_producer);
    for (auto &[result, expected] : results) {
        EXPECT_EQ(result.get(), expected);
    }
}

} // namespace
} // namespace taskforge
