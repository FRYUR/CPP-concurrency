#include "taskforge/thread_pool.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace taskforge {
namespace {

thread_local const ThreadPool *current_worker_pool = nullptr;

class TaskQueue {
  public:
    bool push(std::unique_ptr<detail::Task> task) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            tasks_.push_back(std::move(task));
        }
        condition_.notify_one();
        return true;
    }

    bool pop(std::unique_ptr<detail::Task> &task) {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return closed_ || !tasks_.empty(); });
        if (tasks_.empty()) {
            return false;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        condition_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::unique_ptr<detail::Task>> tasks_;
    bool closed_ = false;
};

} // namespace

struct ThreadPool::Impl {
    Impl(std::size_t worker_count, const ThreadPool *owner) : owner_(owner) {
        workers_.reserve(worker_count);
        try {
            for (std::size_t index = 0; index < worker_count; ++index) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            queue_.close();
            for (auto &worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    void enqueue(std::unique_ptr<detail::Task> task) {
        if (!queue_.push(std::move(task))) {
            throw std::runtime_error("cannot submit a task after shutdown has started");
        }
    }

    void shutdown() {
        std::lock_guard shutdown_lock(shutdown_mutex_);
        queue_.close();
        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void worker_loop() {
        current_worker_pool = owner_;
        std::unique_ptr<detail::Task> task;
        while (queue_.pop(task)) {
            task->run();
            task.reset();
        }
        current_worker_pool = nullptr;
    }

    TaskQueue queue_;
    std::vector<std::jthread> workers_;
    std::mutex shutdown_mutex_;
    const ThreadPool *owner_;
};

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("ThreadPool requires at least one worker");
    }
    impl_ = std::make_unique<Impl>(worker_count, this);
}

ThreadPool::~ThreadPool() {
    if (impl_) {
        shutdown();
    }
}

void ThreadPool::enqueue_task(std::unique_ptr<detail::Task> task) {
    impl_->enqueue(std::move(task));
}

void ThreadPool::shutdown() {
    if (current_worker_pool == this) {
        throw std::logic_error("shutdown cannot be called from a worker in the same pool");
    }
    impl_->shutdown();
}

std::size_t ThreadPool::worker_count() const noexcept {
    return impl_->workers_.size();
}

} // namespace taskforge
