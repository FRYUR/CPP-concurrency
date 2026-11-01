# TaskForge

TaskForge is a C++20 learning and portfolio project for comparing a fixed-size
thread pool with a mutex-protected work-stealing pool. It focuses on explicit
task ownership, futures, exception propagation, and shutdown behavior. It is
not a production-ready scheduler and does not provide lock-free queues,
cooperative future waiting, cancellation, priorities, or real-time guarantees.

## Build and run

Configure, build, and test the Debug preset:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Use the Release preset with:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The Release preset also builds the benchmark executable. See
[docs/BENCHMARKS.md](docs/BENCHMARKS.md) for run parameters and how to record
results:

```sh
./build/release/taskforge_benchmarks --benchmark_repetitions=5
```

Separate sanitizer configurations are available on supported Clang/GCC
toolchains:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

AddressSanitizer and ThreadSanitizer are mutually exclusive configurations.
ASan can detect certain memory errors in executed paths; TSan can report
data races observed during an instrumented run. Neither proves correctness or
covers every execution schedule.

GoogleTest 1.15.2 is fetched by CMake when testing is enabled. The three
examples are built with the library:

```sh
./build/debug/taskforge_independent_tasks
./build/debug/taskforge_uneven_cpu_tasks
./build/debug/taskforge_parallel_reduction
```

## Minimal usage

```cpp
#include "taskforge/thread_pool.hpp"

#include <iostream>

int main() {
    taskforge::ThreadPool pool(2);
    auto answer = pool.submit([](int left, int right) { return left + right; },
                              20, 22);
    std::cout << answer.get() << '\n';
    pool.shutdown();
}
```

`submit` returns a `std::future<R>`. Arguments are stored by value and passed
to the callable as rvalues; use `std::ref` for explicit reference semantics.
Move-only callables and arguments, `void` results, and exceptions are
supported. A task exception is rethrown when its future is read.

## Shutdown and thread safety

Submissions and shutdown are synchronized. A submission racing with shutdown
is either accepted and executed exactly once or rejected with
`std::runtime_error`; accepted tasks are not silently discarded. `shutdown()`
stops accepting tasks, drains accepted work, and joins workers. Repeated calls
are safe. A zero-worker pool throws `std::invalid_argument`.

Calling `shutdown()` from one of that pool's workers throws
`std::logic_error`. Pool destruction must happen outside its own workers.
Separate pool instances have separate queues and worker state.

Do not block on a future from a worker when its completion depends on work
scheduled to the same saturated pool. This can deadlock: TaskForge does not
implement cooperative waiting or help-while-waiting behavior. The examples
collect futures from outside the worker tasks.

## Architecture

`ThreadPool` uses one closeable, mutex-protected FIFO queue. Its workers wait
on a condition variable and drain the queue before exiting.
`WorkStealingThreadPool` uses a mutex-protected deque per worker plus a global
injection queue for external submissions. Owners take from the back; idle
workers try the injection queue and steal from the front of other workers'
deques. A wake-generation predicate prevents a submission notification from
being lost while a worker transitions to parking. See [docs/DESIGN.md](docs/DESIGN.md)
for the synchronization protocol.
