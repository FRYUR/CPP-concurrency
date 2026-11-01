#pragma once

#include <functional>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace taskforge::detail {

class Task {
  public:
    virtual ~Task() = default;
    virtual void run() = 0;
};

template <typename F> class TaskModel final : public Task {
  public:
    explicit TaskModel(F callable) : callable_(std::move(callable)) {}

    void run() override {
        std::invoke(callable_);
    }

  private:
    F callable_;
};

template <typename F, typename... Args> auto make_submission(F &&function, Args &&...args) {
    using Function = std::decay_t<F>;
    using Result = std::invoke_result_t<Function, std::decay_t<Args>...>;
    using StoredArguments = std::tuple<std::decay_t<Args>...>;

    auto bound = [callable = Function(std::forward<F>(function)),
                  arguments = StoredArguments(std::forward<Args>(args)...)]() mutable -> Result {
        return std::apply(
            [&callable](auto &&...unpacked) -> Result {
                return std::invoke(std::move(callable),
                                   std::forward<decltype(unpacked)>(unpacked)...);
            },
            std::move(arguments));
    };

    std::packaged_task<Result()> task(std::move(bound));
    auto result = task.get_future();
    using StoredTask = std::packaged_task<Result()>;
    std::unique_ptr<Task> erased_task = std::make_unique<TaskModel<StoredTask>>(std::move(task));
    return std::pair{std::move(result), std::move(erased_task)};
}

} // namespace taskforge::detail
