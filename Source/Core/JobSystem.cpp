// Core/JobSystem.cpp - Win32 fiber implementation of the job system.
//
// Concurrency model:
//   * One global queue per priority + one "ready" queue of resumable fibers,
//     all guarded by `qlock`. A free-fiber pool guarded by `flock`. A counter
//     pool guarded by `clock`.
//   * Worker threads ConvertThreadToFiber (their "scheduler fiber") and loop:
//     resume a ready fiber, else pull a job and run it on a free fiber, else
//     sleep on `qcv`.
//   * The race-free wait: a fiber that must wait sets its disposition and
//     switches to the scheduler; only AFTER the fiber is fully off-CPU does the
//     scheduler register it as a waiter (under the counter's lock). The counter
//     decrement also takes that lock, so a wake can never be lost, and a fiber
//     can never be resumed on one thread while still running on another.
#include "Core/JobSystem.h"

#include "Core/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace hbe::jobs {

namespace {

// 128 fibers comfortably covers this engine's nesting depth (a handful of
// concurrent ParallelFors, each at most `workers` deep). The pool is bounded:
// if every fiber were parked waiting on jobs that themselves need a free fiber
// to run, it could stall - which does not happen at these depths.
constexpr u32 kFiberCount     = 128;
constexpr u32 kFiberStackSize = 512 * 1024; // generous: jobs may parse/build data

enum class Disposition : u8 {
    Running, // currently executing (transient)
    ToPool,  // job finished; return to the free pool
    ToWait,  // parked on waitCounter; register as a waiter
};

struct Fiber {
    void*       handle = nullptr; // Win32 fiber
    JobDecl     job;              // job to run when switched in
    Counter*    completion = nullptr; // decremented after `job` runs
    Counter*    waitCounter = nullptr; // counter being waited on (ToWait)
    u32         waitTarget = 0;
    Disposition disp = Disposition::ToPool;
    u32         index = 0;
};

struct QueuedJob {
    JobDecl  decl;
    Counter* completion = nullptr;
};

} // namespace

// Defined at namespace scope so the opaque `Counter*` in the public header
// refers to this type.
struct Counter {
    std::atomic<u32>     value{0};
    std::mutex           lock;       // guards `waiters`; paired with value RMW
    std::vector<Fiber*>  waiters;    // parked fibers (each with its own target)
    std::condition_variable cv;      // external-thread waiters
};

namespace {

struct System {
    std::atomic<bool>        running{false};
    u32                      workerCount = 0;
    std::vector<std::thread> workers;

    // Work + ready queues (one lock; both are touched by the scheduler).
    std::mutex              qlock;
    std::condition_variable qcv;
    std::deque<QueuedJob>   queues[3];
    std::deque<Fiber*>      ready;

    // Fiber pool.
    std::mutex            flock;
    std::vector<Fiber*>   freeFibers;
    std::vector<Fiber*>   allFibers;

    // Counter pool.
    std::mutex            clock;
    std::vector<Counter*> counterFree;
    std::vector<Counter*> counterAll;
};

System* g = nullptr;

// Per-thread fiber context. A fiber may migrate between worker threads, so
// these always describe the thread the fiber is CURRENTLY running on; the
// scheduler keeps `t_active` in sync by setting it immediately before each
// switch into a fiber.
thread_local void*  t_scheduler = nullptr; // this worker's scheduler fiber
thread_local Fiber* t_active    = nullptr; // fiber running on this thread now
thread_local u32    t_worker    = kNotAWorker;
thread_local bool   t_isWorker  = false;

// -- Counter pool --------------------------------------------------------------

Counter* AllocCounter(u32 initial) {
    Counter* c = nullptr;
    {
        std::lock_guard<std::mutex> lk(g->clock);
        if (!g->counterFree.empty()) {
            c = g->counterFree.back();
            g->counterFree.pop_back();
        }
    }
    if (!c) {
        c = new Counter();
        std::lock_guard<std::mutex> lk(g->clock);
        g->counterAll.push_back(c);
    }
    c->value.store(initial, std::memory_order_relaxed);
    c->waiters.clear();
    return c;
}

void FreeCounter(Counter* c) {
    std::lock_guard<std::mutex> lk(g->clock);
    g->counterFree.push_back(c);
}

// -- Queues --------------------------------------------------------------------

void PushReady(Fiber* f) {
    {
        std::lock_guard<std::mutex> lk(g->qlock);
        g->ready.push_back(f);
    }
    g->qcv.notify_one();
}

// Picks the next thing for a scheduler to do: a resumable fiber first (finishing
// in-flight work beats starting new work), else the highest-priority job.
bool NextWork(Fiber*& outReady, QueuedJob& outJob) {
    std::lock_guard<std::mutex> lk(g->qlock);
    if (!g->ready.empty()) {
        outReady = g->ready.front();
        g->ready.pop_front();
        return true;
    }
    for (auto& q : g->queues) {
        if (!q.empty()) {
            outJob = q.front();
            q.pop_front();
            return true;
        }
    }
    return false;
}

Fiber* AllocFreeFiber() {
    std::lock_guard<std::mutex> lk(g->flock);
    if (g->freeFibers.empty()) return nullptr;
    Fiber* f = g->freeFibers.back();
    g->freeFibers.pop_back();
    return f;
}

void ReturnFreeFiber(Fiber* f) {
    {
        std::lock_guard<std::mutex> lk(g->flock);
        g->freeFibers.push_back(f);
    }
    // A scheduler may have been spinning because no fiber was free; nudge it.
    g->qcv.notify_one();
}

// -- Counter decrement / waiter wake ------------------------------------------

void DecrementCounter(Counter* c) {
    std::vector<Fiber*> woken;
    {
        std::lock_guard<std::mutex> lk(c->lock);
        const u32 prev = c->value.fetch_sub(1, std::memory_order_acq_rel);
        const u32 now = prev - 1;
        for (auto it = c->waiters.begin(); it != c->waiters.end();) {
            if (now <= (*it)->waitTarget) {
                woken.push_back(*it);
                it = c->waiters.erase(it);
            } else {
                ++it;
            }
        }
        c->cv.notify_all(); // external-thread waiters re-check the predicate
    }
    for (Fiber* f : woken) PushReady(f);
}

// Registers a just-parked fiber as a waiter. Re-checks the counter under the
// lock so a decrement that landed during the switch is not missed.
void RegisterWaiter(Fiber* f) {
    Counter* c = f->waitCounter;
    bool satisfied = false;
    {
        std::lock_guard<std::mutex> lk(c->lock);
        if (c->value.load(std::memory_order_acquire) <= f->waitTarget) {
            satisfied = true;
        } else {
            c->waiters.push_back(f);
        }
    }
    if (satisfied) PushReady(f); // already done - resume immediately
}

// -- Fiber entry + scheduler ---------------------------------------------------

void CALLBACK FiberProc(void*) {
    for (;;) {
        Fiber* self = t_active; // set by the scheduler before switching in
        if (self->job.entry) self->job.entry(self->job.arg);
        if (self->completion) DecrementCounter(self->completion);
        self->job = {};
        self->completion = nullptr;
        self->disp = Disposition::ToPool;
        SwitchToFiber(t_scheduler); // hand control back; scheduler pools us
        // Resumed here when reused with a fresh job (see scheduler).
    }
}

// Handles the fiber that just switched back to this scheduler.
void AfterSwitch(Fiber* f) {
    switch (f->disp) {
        case Disposition::ToPool: ReturnFreeFiber(f); break;
        case Disposition::ToWait: RegisterWaiter(f); break;
        default: break;
    }
    t_active = nullptr;
}

void Scheduler() {
    while (g->running.load(std::memory_order_relaxed)) {
        Fiber* ready = nullptr;
        QueuedJob job;
        if (!NextWork(ready, job)) {
            std::unique_lock<std::mutex> lk(g->qlock);
            g->qcv.wait_for(lk, std::chrono::milliseconds(2), [] {
                return !g->running.load(std::memory_order_relaxed) ||
                       !g->ready.empty() || !g->queues[0].empty() ||
                       !g->queues[1].empty() || !g->queues[2].empty();
            });
            continue;
        }

        Fiber* f = ready;
        if (!f) {
            f = AllocFreeFiber();
            if (!f) {
                // Out of fibers: requeue the job and let a resume free one up.
                {
                    std::lock_guard<std::mutex> lk(g->qlock);
                    g->queues[1].push_front(job);
                }
                std::this_thread::yield();
                continue;
            }
            f->job = job.decl;
            f->completion = job.completion;
        }
        f->disp = Disposition::Running;
        t_active = f;
        SwitchToFiber(f->handle);
        AfterSwitch(f);
    }
}

void WorkerMain(u32 index) {
    t_isWorker = true;
    t_worker = index;
    t_scheduler = ConvertThreadToFiber(nullptr);
    Scheduler();
    ConvertFiberToThread();
}

// -- Self-test ----------------------------------------------------------------

// Exercises the full machinery once at startup: a flat ParallelFor, plus jobs
// that themselves wait on nested ParallelFors (the fiber park/resume path).
bool SelfTest() {
    constexpr u32 kN = 100000;
    std::atomic<u64> sum{0};
    ParallelFor(kN, 1024, [&](u32 b, u32 e) {
        u64 local = 0;
        for (u32 i = b; i < e; ++i) local += i;
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    const u64 expected = static_cast<u64>(kN) * (kN - 1) / 2;
    if (sum.load() != expected) return false;

    // Nested: each outer job runs an inner ParallelFor and waits on it. This
    // only completes if parked fibers resume correctly (across threads).
    std::atomic<u32> nested{0};
    ParallelFor(64, 1, [&](u32 b, u32 e) {
        for (u32 i = b; i < e; ++i) {
            std::atomic<u32> inner{0};
            ParallelFor(50, 8, [&](u32 ib, u32 ie) {
                inner.fetch_add(ie - ib, std::memory_order_relaxed);
            });
            if (inner.load() == 50) nested.fetch_add(1, std::memory_order_relaxed);
        }
    });
    return nested.load() == 64;
}

} // namespace

// -- Public API ----------------------------------------------------------------

void Initialize(u32 workerCount) {
    if (g) return;
    g = new System();

    if (workerCount == 0) {
        const u32 hw = std::thread::hardware_concurrency();
        workerCount = hw > 1 ? hw - 1 : 1;
    }
    g->workerCount = workerCount;

    g->allFibers.reserve(kFiberCount);
    g->freeFibers.reserve(kFiberCount);
    for (u32 i = 0; i < kFiberCount; ++i) {
        Fiber* f = new Fiber();
        f->index = i;
        f->handle = CreateFiber(kFiberStackSize, &FiberProc, nullptr);
        g->allFibers.push_back(f);
        g->freeFibers.push_back(f);
    }

    g->running.store(true, std::memory_order_relaxed);
    g->workers.reserve(workerCount);
    for (u32 i = 0; i < workerCount; ++i) g->workers.emplace_back(WorkerMain, i);

    HBE_INFO("JobSystem: {} workers, {} fibers ({} KB stacks).", workerCount,
             kFiberCount, kFiberStackSize / 1024);

    if (SelfTest()) {
        HBE_INFO("JobSystem: self-test passed.");
    } else {
        HBE_ERROR("JobSystem: self-test FAILED - parallelism is unreliable.");
    }
}

void Shutdown() {
    if (!g) return;
    g->running.store(false, std::memory_order_relaxed);
    g->qcv.notify_all();
    for (std::thread& t : g->workers)
        if (t.joinable()) t.join();

    for (Fiber* f : g->allFibers) {
        if (f->handle) DeleteFiber(f->handle);
        delete f;
    }
    for (Counter* c : g->counterAll) delete c;
    delete g;
    g = nullptr;
}

bool IsInitialized() { return g != nullptr; }
u32  WorkerCount() { return g ? g->workerCount : 0; }
u32  ThisWorkerIndex() { return t_worker; }

Counter* Kick(const JobDecl* decls, u32 count, Priority prio) {
    Counter* c = AllocCounter(count);
    if (count == 0) return c;
    {
        std::lock_guard<std::mutex> lk(g->qlock);
        std::deque<QueuedJob>& q = g->queues[static_cast<int>(prio)];
        for (u32 i = 0; i < count; ++i) q.push_back({decls[i], c});
    }
    if (count == 1) g->qcv.notify_one();
    else           g->qcv.notify_all();
    return c;
}

Counter* Kick(JobEntry entry, void* arg, Priority prio) {
    const JobDecl decl{entry, arg};
    return Kick(&decl, 1, prio);
}

void RunDetached(JobEntry entry, void* arg, Priority prio) {
    {
        std::lock_guard<std::mutex> lk(g->qlock);
        g->queues[static_cast<int>(prio)].push_back({{entry, arg}, nullptr});
    }
    g->qcv.notify_one();
}

void Wait(Counter* counter, u32 target) {
    if (!counter) return;

    if (t_isWorker && t_active) {
        // Fiber path: park (and let the worker do other jobs) until satisfied.
        if (counter->value.load(std::memory_order_acquire) > target) {
            Fiber* self = t_active;
            self->waitCounter = counter;
            self->waitTarget = target;
            self->disp = Disposition::ToWait;
            SwitchToFiber(t_scheduler); // scheduler registers us; resumes later
        }
    } else {
        // External thread (e.g. the main thread): block on the counter's CV.
        std::unique_lock<std::mutex> lk(counter->lock);
        counter->cv.wait(lk, [&] {
            return counter->value.load(std::memory_order_acquire) <= target;
        });
    }

    if (target == 0) FreeCounter(counter);
}

void ParallelFor(u32 count, u32 group,
                 const std::function<void(u32, u32)>& fn, Priority prio) {
    if (count == 0) return;
    if (!g || g->workerCount == 0) { // not initialized: run inline
        fn(0, count);
        return;
    }
    group = std::max(1u, group);
    const u32 jobCount = (count + group - 1) / group;

    // Per-job ranges live on this stack frame; Wait() below keeps them alive
    // until every job has run.
    struct Range {
        const std::function<void(u32, u32)>* fn;
        u32 begin;
        u32 end;
    };
    std::vector<Range> ranges(jobCount);
    std::vector<JobDecl> decls(jobCount);
    for (u32 i = 0; i < jobCount; ++i) {
        ranges[i] = {&fn, i * group, std::min(count, (i + 1) * group)};
        decls[i].entry = [](void* a) {
            Range* r = static_cast<Range*>(a);
            (*r->fn)(r->begin, r->end);
        };
        decls[i].arg = &ranges[i];
    }
    Wait(Kick(decls.data(), jobCount, prio), 0);
}

} // namespace hbe::jobs
