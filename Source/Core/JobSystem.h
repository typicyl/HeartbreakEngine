// Core/JobSystem.h - a fiber-based job system (Naughty Dog "Parallelizing the
// Naughty Dog Engine Using Fibers", GDC 2015).
//
// Worker threads (one per logical core by default) are converted to fibers and
// run a scheduler loop. Jobs run on a pool of pre-allocated fibers. A job can
// WAIT on a Counter mid-execution WITHOUT blocking its worker thread: the fiber
// is parked, the worker picks up other work, and the parked fiber is resumed -
// possibly on a DIFFERENT thread - once the counter is satisfied. This makes
// fine-grained, deeply-nested parallelism cheap (a wait costs a fiber switch,
// not a thread block), which is what the rest of the engine builds on
// (animation, scene streaming, culling, motion matching).
//
// Usage:
//     jobs::Initialize();                          // engine startup
//     jobs::ParallelFor(n, group, [&](u32 b, u32 e){ ... });  // blocks here
//     ...
//     jobs::Shutdown();                            // engine teardown
//
// Or the lower-level batch API:
//     jobs::Counter* c = jobs::Kick(decls, count);
//     jobs::Wait(c);   // parks the calling fiber (or blocks an external thread)
#pragma once

#include "Core/Types.h"

#include <functional>

namespace hbe::jobs {

// A job's work function. A raw function pointer + opaque argument (rather than
// std::function) keeps the per-job record tiny and trivially copyable, which
// matters when thousands are queued; capture state via the argument instead.
using JobEntry = void (*)(void* arg);

// Queue priority. High-priority jobs are picked before lower ones.
enum class Priority : u8 { High = 0, Normal = 1, Low = 2 };

// A single unit of work.
struct JobDecl {
    JobEntry entry = nullptr;
    void*    arg   = nullptr;
};

// An atomic completion counter. Opaque (pooled internally); obtained from Kick
// and consumed by Wait. Never construct or dereference one directly.
struct Counter;

// Starts the worker threads and fiber pool. `workerCount` of 0 uses
// (hardware_concurrency - 1), leaving a core for the main thread. Safe to call
// once; a second call is a no-op. Runs a self-test and logs the result.
void Initialize(u32 workerCount = 0);

// Stops the workers and releases the fibers. No jobs may be in flight.
void Shutdown();

bool IsInitialized();
u32  WorkerCount();

// Index of the calling worker (0..WorkerCount-1), or kNotAWorker for the main
// thread / any thread that is not a job-system worker.
static constexpr u32 kNotAWorker = 0xFFFFFFFFu;
u32 ThisWorkerIndex();

// Submits `count` jobs and returns a counter initialized to `count`. The
// counter is decremented as each job finishes; Wait() on it to know they are
// all done. The decls array need only live until Kick returns (it is copied).
Counter* Kick(const JobDecl* decls, u32 count, Priority prio = Priority::Normal);

// Convenience: submit a single job.
Counter* Kick(JobEntry entry, void* arg, Priority prio = Priority::Normal);

// Submits a single fire-and-forget job with no completion counter (nothing to
// wait on, nothing to free). Use for background work whose result is observed
// another way - e.g. an atomic the job stores into. StreamingWorld loads cells
// this way.
void RunDetached(JobEntry entry, void* arg, Priority prio = Priority::Normal);

// Waits until *counter <= target, then frees the counter when target is 0 (the
// usual "kick a batch, wait once" pattern - do not Wait on the same counter
// twice). Called from a worker fiber, the worker keeps running other jobs while
// this one is parked; called from an external thread (e.g. the main thread), it
// blocks that thread.
void Wait(Counter* counter, u32 target = 0);

// Splits [0, count) into ceil(count/group) jobs, each invoking fn(begin, end)
// over its sub-range, and waits for all of them. Safe to call from the main
// thread or from inside another job (nested parallelism). Falls back to a
// direct serial call when the system is not initialized.
void ParallelFor(u32 count, u32 group,
                 const std::function<void(u32 begin, u32 end)>& fn,
                 Priority prio = Priority::Normal);

} // namespace hbe::jobs
