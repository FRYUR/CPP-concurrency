#include "taskforge/work_stealing_pool.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <numeric>
#include <vector>

int main() {
    constexpr std::size_t element_count = 1U << 20;
    constexpr std::size_t chunk_size = 1U << 16;
    std::vector<std::uint64_t> values(element_count);
    for (std::size_t index = 0; index < values.size(); ++index) {
        values[index] = static_cast<std::uint64_t>(index % 97) + 1;
    }

    const auto sequential = std::accumulate(values.begin(), values.end(), std::uint64_t{0});
    taskforge::WorkStealingThreadPool pool(4);
    std::vector<std::future<std::uint64_t>> partials;

    for (std::size_t begin = 0; begin < values.size(); begin += chunk_size) {
        const auto end = std::min(begin + chunk_size, values.size());
        partials.push_back(pool.submit([&values, begin, end] {
            return std::accumulate(values.begin() + begin, values.begin() + end, std::uint64_t{0});
        }));
    }

    std::uint64_t parallel = 0;
    for (auto &partial : partials) {
        parallel += partial.get();
    }
    pool.shutdown();

    if (parallel != sequential) {
        std::cerr << "parallel reduction does not match sequential result\n";
        return 1;
    }

    std::cout << "Verified sum of " << values.size() << " values: " << parallel << '\n';
}
