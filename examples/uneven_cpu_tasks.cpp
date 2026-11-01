#include "taskforge/work_stealing_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <vector>

namespace {

std::uint64_t cpu_work(std::uint64_t iterations) {
    std::uint64_t value = 1;
    for (std::uint64_t index = 0; index < iterations; ++index) {
        value = value * 2862933555777941757ULL + 3037000493ULL + index;
    }
    return value;
}

} // namespace

int main() {
    constexpr std::size_t worker_count = 4;
    const std::vector<std::uint64_t> workloads{120'000, 1'400'000, 280'000, 950'000,
                                               180'000, 1'100'000, 420'000, 700'000};
    taskforge::WorkStealingThreadPool pool(worker_count);
    std::vector<std::future<std::uint64_t>> results;
    results.reserve(workloads.size());

    for (const auto iterations : workloads) {
        results.push_back(pool.submit([iterations] { return cpu_work(iterations); }));
    }

    for (std::size_t index = 0; index < workloads.size(); ++index) {
        const auto actual = results[index].get();
        const auto expected = cpu_work(workloads[index]);
        if (actual != expected) {
            std::cerr << "checksum mismatch for task " << index << '\n';
            return 1;
        }
        std::cout << "task " << index << ": " << workloads[index] << " iterations, checksum "
                  << actual << '\n';
    }

    pool.shutdown();
    std::cout << "All uneven task checksums match sequential results.\n";
}
