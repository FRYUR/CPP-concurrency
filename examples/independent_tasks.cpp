#include "taskforge/thread_pool.hpp"

#include <cstdint>
#include <future>
#include <iostream>
#include <vector>

int main() {
    taskforge::ThreadPool pool(4);
    std::vector<std::future<std::uint64_t>> results;

    for (std::uint64_t value = 1; value <= 8; ++value) {
        results.push_back(pool.submit([value] { return value * value + 3; }));
    }

    for (std::uint64_t value = 1; value <= results.size(); ++value) {
        const auto result = results[value - 1].get();
        const auto expected = value * value + 3;
        if (result != expected) {
            std::cerr << "unexpected result for input " << value << '\n';
            return 1;
        }
        std::cout << value << " -> " << result << '\n';
    }

    pool.shutdown();
}
