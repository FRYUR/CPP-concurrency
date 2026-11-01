#pragma once

#include <cstddef>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "taskforge/detail/task.hpp"

namespace taskforge {

class WorkStealingThreadPool {
  public:
    // Destruction must happen outside this pool's workers. Zero workers is invalid.
    explicit WorkStealingThreadPool(std::size_t worker_count);
    ~WorkStealingThreadPool();

    WorkStealingThreadPool(const WorkStealingThreadPool &) = delete;
    WorkStealingThreadPool &operator=(const WorkStealingThreadPool &) = delete;
    WorkStealingThreadPool(WorkStealingThreadPool &&) = delete;
    WorkStealingThreadPool &operator=(WorkStealingThreadPool &&) = delete;

    // Arguments are stored by value and passed to the callable as rvalues.
    // Use std::ref when a callable must receive a reference.
    // Throws std::runtime_error if shutdown has already closed the pool.
    template <typename F, typename... Args>
    auto submit(F &&function, Args &&...args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
        auto [result, task] =
            detail::make_submission(std::forward<F>(function), std::forward<Args>(args)...);
        enqueue_task(std::move(task));
        return std::move(result);
    }

    // Stops accepting submissions, drains accepted tasks, and joins all workers.
    // Calling this from one of this pool's workers throws std::logic_error.
    void shutdown();

    [[nodiscard]] std::size_t worker_count() const noexcept;

  private:
    struct Impl;

    void enqueue_task(std::unique_ptr<detail::Task> task);

    std::unique_ptr<Impl> impl_;
};

} // namespace taskforge
