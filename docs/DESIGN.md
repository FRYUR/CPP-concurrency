# TaskForge Design

## Baseline thread pool

The baseline pool uses one mutex-protected FIFO queue. Submissions and queue
closure are serialized by the queue mutex. Workers wait on a condition
variable, execute queued tasks, and exit only after the queue is closed and
empty. Shutdown does not request jthread stops: it closes the queue, wakes all
workers, and joins them after accepted work has drained.

## Work-stealing synchronization protocol

This protocol was recorded before implementing the work-stealing pool and is
the synchronization protocol used by this version.

### Why mutex-protected deques

The pool prioritizes a small, auditable synchronization surface. A mutex
protects each deque's insertion and removal operations, so publication and
ownership transfer are straightforward to reason about and inspect with
ThreadSanitizer. The tradeoff is lock contention when many workers compete for
the same deque. A lock-free deque would require careful memory ordering and
safe reclamation, which would add complexity without being needed to
demonstrate work stealing.

### Queue ownership and locking

- Each worker has a mutex-protected deque. Its owner pushes and pops at the
  back; thieves take from the front.
- A separate mutex-protected injection queue receives submissions from
  threads that are not workers of this pool.
- A worker submitting to this same pool places its task on its own deque.
- A worker checks its own deque, then the injection queue, then other workers'
  deques in a rotating victim order.
- After checking its own deque, a worker removes the newest local task from
  the back. A thief removes the oldest task from the front. Each operation
  takes only that queue's mutex.
- An operation holds at most one queue mutex. Submission may hold the pool
  state mutex before taking one queue mutex. Workers release each queue mutex
  before acquiring the state mutex or trying another queue. No code takes the
  state mutex while holding a queue mutex, so there is no reverse lock order.

### Publication, wakeups, and parking

- The state mutex protects whether submissions are accepted, the outstanding
  task count, and a wake generation.
- Submission holds the state mutex while checking acceptance and publishing
  into exactly one queue. Only after publication succeeds does it increment
  the outstanding count and wake generation. It then releases the locks and
  notifies a worker.
- Before probing queues, a worker snapshots the wake generation under the state
  mutex. If it finds no task, it waits with a predicate that checks for a
  generation change or for shutdown with no outstanding work.
- Because publication changes the generation under the same mutex used by the
  wait predicate, a notification between the queue probe and parking cannot
  be lost. If work arrived before the snapshot, the queue probe sees it unless
  another worker already took it.
- The condition-variable wait is predicate-based, so spurious wakeups return
  to the work search rather than being treated as work.

### Shutdown and work accounting

- The outstanding count includes both queued and running accepted tasks. It is
  incremented on successful publication and decremented only after execution
  completes; taking a task does not change it.
- Shutdown uses the state mutex to stop acceptance and advance the wake
  generation, then notifies all workers. The mutex serializes this transition
  with publication, so each racing submission is either fully accepted and
  counted or rejected.
- Workers continue taking tasks after closure and exit only when the pool is
  closed and the outstanding count reaches zero. Shutdown waits for that count
  to reach zero and joins every worker. It does not request jthread stops.
- The serialized state transition makes `submit` racing with `shutdown`
  linearizable: a task is fully published and counted before closure, or the
  submitter receives an exception. Workers cannot observe an accepted task
  without its accounting update because the submitter still holds the state
  mutex while publishing and incrementing the count.

Explicit shutdown from one of the pool's workers is rejected to avoid
self-joining. Destruction from a worker is prohibited; owners must keep the
pool alive until worker tasks finish. Waiting inside a pool worker on a future
that depends on work scheduled to the same saturated pool can deadlock. There
is no cooperative waiting mechanism.

## Future experiments

- Compare a lock-free deque with the mutex version only after specifying its
  memory reclamation and memory-ordering model; this may reduce contention but
  substantially increases correctness and review costs.
- Add cancellation or priorities with explicit rules for queued tasks, running
  tasks, and future completion; these features change shutdown semantics.
- Explore cooperative future waiting to mitigate saturated-pool nested waits;
  workers would need to execute other work while waiting, changing fairness
  and task-ordering behavior.
- Measure victim-selection strategies and queue padding before adding
  architecture-specific or cache-layout optimizations.
