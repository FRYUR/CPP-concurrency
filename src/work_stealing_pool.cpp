#include "taskforge/work_stealing_pool.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace taskforge {
namespace {

thread_local const WorkStealingThreadPool *current_work_stealing_pool = nullptr;
thread_local std::size_t current_work_stealing_worker = 0;

class WorkDeque {
  public:
    void push_back(std::unique_ptr<detail::Task> task) {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    bool pop_back(std::unique_ptr<detail::Task> &task) {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
            return false;
        }
        task = std::move(tasks_.back());
        tasks_.pop_back();
        return true;
    }

    bool steal_front(std::unique_ptr<detail::Task> &task) {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
            return false;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
        return true;
    }

    bool pop_front(std::unique_ptr<detail::Task> &task) {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
            return false;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
        return true;
    }

  private:
    std::mutex mutex_;
    std::deque<std::unique_ptr<detail::Task>> tasks_;
};

} // namespace

struct WorkStealingThreadPool::Impl {
    Impl(std::size_t worker_count, const WorkStealingThreadPool *owner) : owner_(owner) {
        worker_queues_.reserve(worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            worker_queues_.push_back(std::make_unique<WorkDeque>());
        }

        workers_.reserve(worker_count);
        try {
            for (std::size_t index = 0; index < worker_count; ++index) {
                workers_.emplace_back([this, index] { worker_loop(index); });
            }
        } catch (...) {
            {
                std::lock_guard lock(state_mutex_);
                accepting_ = false;
                ++wake_generation_;
            }
            work_condition_.notify_all();
            for (auto &worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    void enqueue(std::unique_ptr<detail::Task> task, bool local, std::size_t worker_index) {
        {
            std::lock_guard state_lock(state_mutex_);
            if (!accepting_) {
                throw std::runtime_error("cannot submit a task after shutdown has started");
            }
            if (local) {
                worker_queues_[worker_index]->push_back(std::move(task));
            } else {
                injection_queue_.push_back(std::move(task));
            }
            ++outstanding_tasks_;
            ++wake_generation_;
        }
        work_condition_.notify_one();
    }

    void shutdown() {
        std::lock_guard shutdown_lock(shutdown_mutex_);
        {
            std::lock_guard state_lock(state_mutex_);
            accepting_ = false;
            ++wake_generation_;
        }
        work_condition_.notify_all();

        {
            std::unique_lock state_lock(state_mutex_);
            work_condition_.wait(state_lock, [this] { return outstanding_tasks_ == 0; });
        }

        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    bool try_take_task(std::size_t worker_index, std::size_t &victim_cursor,
                       std::unique_ptr<detail::Task> &task) {
        if (worker_queues_[worker_index]->pop_back(task)) {
            return true;
        }
        if (injection_queue_.pop_front(task)) {
            return true;
        }

        const auto count = worker_queues_.size();
        for (std::size_t offset = 0; offset < count; ++offset) {
            const auto victim = (victim_cursor + offset) % count;
            if (victim != worker_index && worker_queues_[victim]->steal_front(task)) {
                victim_cursor = (victim + 1) % count;
                return true;
            }
        }
        victim_cursor = (victim_cursor + 1) % count;
        return false;
    }

    void complete_task() {
        bool notify_workers = false;
        {
            std::lock_guard state_lock(state_mutex_);
            --outstanding_tasks_;
            if (!accepting_ && outstanding_tasks_ == 0) {
                ++wake_generation_;
                notify_workers = true;
            }
        }
        if (notify_workers) {
            work_condition_.notify_all();
        }
    }

    void worker_loop(std::size_t worker_index) {
        current_work_stealing_pool = owner_;
        current_work_stealing_worker = worker_index;
        auto victim_cursor = (worker_index + 1) % worker_queues_.size();
        std::unique_ptr<detail::Task> task;

        for (;;) {
            std::uint64_t observed_generation;
            {
                std::lock_guard state_lock(state_mutex_);
                observed_generation = wake_generation_;
            }

            if (try_take_task(worker_index, victim_cursor, task)) {
                task->run();
                task.reset();
                complete_task();
                continue;
            }

            std::unique_lock state_lock(state_mutex_);
            work_condition_.wait(state_lock, [this, observed_generation] {
                return wake_generation_ != observed_generation ||
                       (!accepting_ && outstanding_tasks_ == 0);
            });
            if (!accepting_ && outstanding_tasks_ == 0) {
                break;
            }
        }

        current_work_stealing_pool = nullptr;
        current_work_stealing_worker = 0;
    }

    std::vector<std::unique_ptr<WorkDeque>> worker_queues_;
    WorkDeque injection_queue_;
    std::vector<std::jthread> workers_;
    std::mutex state_mutex_;
    std::condition_variable work_condition_;
    std::mutex shutdown_mutex_;
    std::size_t outstanding_tasks_ = 0;
    std::uint64_t wake_generation_ = 0;
    bool accepting_ = true;
    const WorkStealingThreadPool *owner_;
};

WorkStealingThreadPool::WorkStealingThreadPool(std::size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("WorkStealingThreadPool requires at least one worker");
    }
    impl_ = std::make_unique<Impl>(worker_count, this);
}

WorkStealingThreadPool::~WorkStealingThreadPool() {
    if (impl_) {
        shutdown();
    }
}

void WorkStealingThreadPool::enqueue_task(std::unique_ptr<detail::Task> task) {
    const bool local = current_work_stealing_pool == this;
    impl_->enqueue(std::move(task), local, current_work_stealing_worker);
}

void WorkStealingThreadPool::shutdown() {
    if (current_work_stealing_pool == this) {
        throw std::logic_error("shutdown cannot be called from a worker in the same pool");
    }
    impl_->shutdown();
}

std::size_t WorkStealingThreadPool::worker_count() const noexcept {
    return impl_->worker_queues_.size();
}

} // namespace taskforge
