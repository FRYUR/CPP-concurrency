# Benchmarking

TaskForge benchmarks sequential execution, the baseline `ThreadPool`, and
`WorkStealingThreadPool` for tiny tasks, medium CPU-bound tasks, and uneven
CPU-bound tasks. The benchmark target is restricted to the Release build with
sanitizers disabled.

## Run

```sh
cmake --preset release
cmake --build --preset release --target taskforge_benchmarks
./build/release/taskforge_benchmarks \
  --benchmark_min_warmup_time=0.25 \
  --benchmark_min_time=0.5s \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  --benchmark_format=json \
  --benchmark_out=build/release/benchmark-results.json
```

Google Benchmark 1.9.1 is fetched by the Release preset. Warmup and repeated
measurements are controlled by the command-line options above.

## Workloads and timing boundaries

| Workload | Task count | Work per task | Worker counts |
|---|---:|---|---|
| Tiny | 2,048 | Small integer result | 1, 4, 8 |
| Medium | 64 | 12,000 dependent integer iterations | 1, 4, 8 |
| Uneven | 64 | Deterministic mix from 1,500 to 100,000 iterations | 1, 4, 8 |

Every benchmark verifies its aggregate result against a precomputed sequential
result and consumes the result with Google Benchmark's optimization barriers.
Pool startup is excluded from steady-state submission measurements. Separate
startup benchmarks time worker-pool construction; shutdown and destruction are
paused out of those startup measurements. Pool shutdown is also paused out of
steady-state measurements.

The sequential benchmarks establish a simple reference, not a promise that
parallel execution wins. Tiny tasks are expected to expose scheduling costs,
and work stealing may be slower for some workloads, worker counts, or
machines. Keep such results rather than omitting unfavorable cases.

## Record the environment

Save the JSON output with the benchmark run and record:

- OS version and architecture (`uname -a`; on macOS, also record the CPU model
  with `sysctl -n machdep.cpu.brand_string`; on Linux, use `lscpu`).
- Compiler and version (`clang++ --version` or `g++ --version`).
- CMake Release compiler flags from `build/release/CMakeCache.txt`, including
  `CMAKE_CXX_FLAGS_RELEASE`.
- The task counts, worker counts, warmup duration, and repetition count shown
  above.

Do not compare results from sanitizer builds or unoptimized Debug builds.
Different machines and compilers can produce different outcomes.

## Results

Measured on 2026-11-01 using the Release preset, Apple Clang 21.0.0, flags
`-O3 -DNDEBUG -std=c++20`, macOS 26.7.1 (arm64), and an Apple M1 with 8 CPUs detected.
The run used a 0.1-second minimum warmup, a 0.2-second minimum per repetition,
and 3 repetitions. Values below are median wall time per iteration in
microseconds. The benchmark runtime could not read the CPU clock rate or set
thread affinity; its frequency metadata is unreliable, and the measurements
should be interpreted as one-machine observations rather than portable
performance claims.

| Workload | Sequential | ThreadPool 1 worker | 4 workers | 8 workers | WorkStealing 1 worker | 4 workers | 8 workers |
|---|---:|---:|---:|---:|---:|---:|---:|
| Tiny, 2,048 tasks | 1.43 | 461.74 | 856.44 | 1,404.73 | 601.16 | 1,263.74 | 2,503.15 |
| Medium, 64 tasks | 741.90 | 803.95 | 239.52 | 201.73 | 800.55 | 244.69 | 209.30 |
| Uneven, 64 tasks | 1,565.54 | 1,620.26 | 480.06 | 362.11 | 1,621.69 | 481.51 | 368.47 |

| Startup-only construction | 1 worker | 4 workers | 8 workers |
|---|---:|---:|---:|
| ThreadPool | 6.15 | 20.58 | 40.33 |
| WorkStealingThreadPool | 6.13 | 19.52 | 38.24 |

Work stealing was slower than the baseline for every tiny-task worker count,
for medium tasks at four and eight workers, and for uneven tasks at all worker
counts. It was marginally faster for medium tasks at one worker. These results
illustrate that work stealing is not consistently faster; repeat the
measurements on the target machine before drawing conclusions.
