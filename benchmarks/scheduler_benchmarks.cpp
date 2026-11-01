#include "taskforge/thread_pool.hpp"
#include "taskforge/work_stealing_pool.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <vector>

namespace {

enum class Workload { Tiny, Medium, Uneven };

std::uint64_t task_iterations(Workload workload, std::size_t index) {
    switch (workload) {
    case Workload::Tiny:
        return 0;
    case Workload::Medium:
        return 12'000;
    case Workload::Uneven:
        switch (index % 8) {
        case 0:
            return 100'000;
        case 1:
            return 1'500;
        case 2:
            return 20'000;
        case 3:
            return 3'000;
        case 4:
            return 60'000;
        case 5:
            return 2'000;
        case 6:
            return 12'000;
        default:
            return 4'000;
        }
    }
    return 0;
}

std::uint64_t run_task(Workload workload, std::size_t index) {
    auto input = static_cast<std::uint64_t>(index);
    benchmark::DoNotOptimize(input);
    if (workload == Workload::Tiny) {
        return input * 17 + 3;
    }

    std::uint64_t value = input + 1;
    const auto iterations = task_iterations(workload, static_cast<std::size_t>(input));
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        value = value * 2862933555777941757ULL + 3037000493ULL + iteration;
    }
    return value;
}

std::uint64_t expected_result(Workload workload, std::size_t task_count) {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < task_count; ++index) {
        result += run_task(workload, index);
    }
    return result;
}

std::uint64_t run_sequential(Workload workload, std::size_t task_count) {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < task_count; ++index) {
        result += run_task(workload, index);
    }
    return result;
}

template <typename Pool>
std::uint64_t run_pool(Pool &pool, Workload workload, std::size_t task_count) {
    std::vector<std::future<std::uint64_t>> results;
    results.reserve(task_count);
    for (std::size_t index = 0; index < task_count; ++index) {
        results.push_back(pool.submit([workload, index] { return run_task(workload, index); }));
    }

    std::uint64_t result = 0;
    for (auto &future : results) {
        result += future.get();
    }
    return result;
}

void set_work_counters(benchmark::State &state, std::size_t task_count) {
    state.counters["tasks_per_iteration"] = static_cast<double>(task_count);
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(task_count));
}

void benchmark_sequential(benchmark::State &state, Workload workload) {
    const auto task_count = static_cast<std::size_t>(state.range(0));
    std::uint64_t expected = 0;
    bool initialized = false;

    for (auto _ : state) {
        if (!initialized) {
            state.PauseTiming();
            expected = expected_result(workload, task_count);
            state.ResumeTiming();
            initialized = true;
        }

        auto result = run_sequential(workload, task_count);
        if (result != expected) {
            state.SkipWithError("sequential result differs from the verified result");
            break;
        }
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
    set_work_counters(state, task_count);
}

template <typename Pool> void benchmark_pool(benchmark::State &state, Workload workload) {
    const auto task_count = static_cast<std::size_t>(state.range(0));
    const auto worker_count = static_cast<std::size_t>(state.range(1));
    std::uint64_t expected = 0;
    bool initialized = false;
    std::optional<Pool> pool;

    for (auto _ : state) {
        if (!initialized) {
            state.PauseTiming();
            expected = expected_result(workload, task_count);
            pool.emplace(worker_count);
            state.ResumeTiming();
            initialized = true;
        }

        auto result = run_pool(*pool, workload, task_count);
        if (result != expected) {
            state.SkipWithError("pool result differs from the verified result");
            break;
        }
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }

    if (pool) {
        pool->shutdown();
    }
    set_work_counters(state, task_count);
    state.counters["workers"] = static_cast<double>(worker_count);
}

void BM_SequentialTiny(benchmark::State &state) {
    benchmark_sequential(state, Workload::Tiny);
}

void BM_SequentialMedium(benchmark::State &state) {
    benchmark_sequential(state, Workload::Medium);
}

void BM_SequentialUneven(benchmark::State &state) {
    benchmark_sequential(state, Workload::Uneven);
}

void BM_ThreadPoolTiny(benchmark::State &state) {
    benchmark_pool<taskforge::ThreadPool>(state, Workload::Tiny);
}

void BM_ThreadPoolMedium(benchmark::State &state) {
    benchmark_pool<taskforge::ThreadPool>(state, Workload::Medium);
}

void BM_ThreadPoolUneven(benchmark::State &state) {
    benchmark_pool<taskforge::ThreadPool>(state, Workload::Uneven);
}

void BM_WorkStealingTiny(benchmark::State &state) {
    benchmark_pool<taskforge::WorkStealingThreadPool>(state, Workload::Tiny);
}

void BM_WorkStealingMedium(benchmark::State &state) {
    benchmark_pool<taskforge::WorkStealingThreadPool>(state, Workload::Medium);
}

void BM_WorkStealingUneven(benchmark::State &state) {
    benchmark_pool<taskforge::WorkStealingThreadPool>(state, Workload::Uneven);
}

template <typename Pool> void benchmark_startup(benchmark::State &state) {
    const auto worker_count = static_cast<std::size_t>(state.range(0));
    for (auto _ : state) {
        std::optional<Pool> pool;
        pool.emplace(worker_count);
        auto actual_worker_count = pool->worker_count();
        benchmark::DoNotOptimize(actual_worker_count);
        state.PauseTiming();
        pool->shutdown();
        pool.reset();
        state.ResumeTiming();
    }
    state.counters["workers"] = static_cast<double>(worker_count);
}

void BM_ThreadPoolStartup(benchmark::State &state) {
    benchmark_startup<taskforge::ThreadPool>(state);
}

void BM_WorkStealingStartup(benchmark::State &state) {
    benchmark_startup<taskforge::WorkStealingThreadPool>(state);
}

BENCHMARK(BM_SequentialTiny)->Arg(2048)->UseRealTime();
BENCHMARK(BM_SequentialMedium)->Arg(64)->UseRealTime();
BENCHMARK(BM_SequentialUneven)->Arg(64)->UseRealTime();

BENCHMARK(BM_ThreadPoolTiny)->Args({2048, 1})->Args({2048, 4})->Args({2048, 8})->UseRealTime();
BENCHMARK(BM_ThreadPoolMedium)->Args({64, 1})->Args({64, 4})->Args({64, 8})->UseRealTime();
BENCHMARK(BM_ThreadPoolUneven)->Args({64, 1})->Args({64, 4})->Args({64, 8})->UseRealTime();

BENCHMARK(BM_WorkStealingTiny)->Args({2048, 1})->Args({2048, 4})->Args({2048, 8})->UseRealTime();
BENCHMARK(BM_WorkStealingMedium)->Args({64, 1})->Args({64, 4})->Args({64, 8})->UseRealTime();
BENCHMARK(BM_WorkStealingUneven)->Args({64, 1})->Args({64, 4})->Args({64, 8})->UseRealTime();

BENCHMARK(BM_ThreadPoolStartup)->Arg(1)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK(BM_WorkStealingStartup)->Arg(1)->Arg(4)->Arg(8)->UseRealTime();

} // namespace
