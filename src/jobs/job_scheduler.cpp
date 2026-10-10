#include "engine/jobs/job_scheduler.hpp"

#include <xenium/michael_scott_queue.hpp>
#include <xenium/reclamation/lock_free_ref_count.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace engine::jobs {
namespace {
using LfrcTraits = xenium::reclamation::lock_free_ref_count_traits<true, 0>;
using LfrcReclaimer = xenium::reclamation::lock_free_ref_count<LfrcTraits>;
thread_local unsigned taskDepth = 0;

struct TaskContext {
    TaskContext() {
        ++taskDepth;
    }
    ~TaskContext() {
        --taskDepth;
    }
};

bool markFailure(std::atomic<std::size_t>& firstFailure, std::size_t task) noexcept {
    auto current = firstFailure.load(std::memory_order_relaxed);
    while (task < current) {
        if (firstFailure.compare_exchange_weak(current, task, std::memory_order_relaxed,
                                               std::memory_order_relaxed))
            return true;
    }
    return false;
}
} // namespace

struct JobScheduler::State {
    std::atomic<std::size_t> references{1};
    std::atomic<std::size_t> activeJobs{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> firstFailure{std::numeric_limits<std::size_t>::max()};
    std::atomic<bool> queueFailure{false};
    std::size_t taskCount = 0;
    std::shared_ptr<const TaskGraph> graph;
    std::shared_ptr<const PreparedTaskGraph::Topology> topology;
    std::unique_ptr<std::atomic<std::size_t>[]> pending;
    std::string compileError;

    void retain() noexcept {
        references.fetch_add(1, std::memory_order_relaxed);
    }
    void release() noexcept {
        if (references.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
    }
    bool complete() const noexcept {
        return activeJobs.load(std::memory_order_acquire) == 0;
    }
};

struct JobScheduler::Impl {
    struct QueuedJob {
        State* state = nullptr;
        TaskId task = 0;
        QueuedJob() = default;
        QueuedJob(State* owner, TaskId id) noexcept : state(owner), task(id) {}
        QueuedJob(const QueuedJob&) = delete;
        QueuedJob& operator=(const QueuedJob&) = delete;
        QueuedJob(QueuedJob&& other) noexcept : state(other.state), task(other.task) {
            other.state = nullptr;
        }
        QueuedJob& operator=(QueuedJob&& other) noexcept {
            if (this != &other) {
                state = other.state;
                task = other.task;
                other.state = nullptr;
            }
            return *this;
        }
    };
    using Queue = xenium::michael_scott_queue<QueuedJob, xenium::policy::reclaimer<LfrcReclaimer>>;

    Queue ready;
    std::atomic<std::size_t> outstanding{0};

    void enqueue(State* state, TaskId id) noexcept {
        state->retain();
        state->activeJobs.fetch_add(1, std::memory_order_relaxed);
        outstanding.fetch_add(1, std::memory_order_relaxed);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
            typename LfrcReclaimer::region_guard guard;
            (void)guard;
            ready.push(QueuedJob(state, id));
        } catch (...) {
            state->queueFailure.store(true, std::memory_order_relaxed);
            markFailure(state->firstFailure, id);
            state->activeJobs.fetch_sub(1, std::memory_order_release);
            outstanding.fetch_sub(1, std::memory_order_release);
            state->release();
        }
#else
        {
            typename LfrcReclaimer::region_guard guard;
            (void)guard;
            ready.push(QueuedJob(state, id));
        }
#endif
    }

    void execute(QueuedJob job) noexcept {
        State* state = job.state;
        if (state == nullptr)
            return;
        const auto id = job.task;
        bool ok = false;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
        try {
            TaskContext context;
            ok = state->graph->tasks()[id].work();
        } catch (...) {
            ok = false;
        }
#else
        {
            TaskContext context;
            ok = state->graph->tasks()[id].work();
        }
#endif
        if (ok) {
            state->completed.fetch_add(1, std::memory_order_relaxed);
            for (const auto next : state->topology->successors[id]) {
                if (state->pending[next].fetch_sub(1, std::memory_order_acq_rel) == 1)
                    enqueue(state, next);
            }
        } else {
            markFailure(state->firstFailure, id);
        }
        state->activeJobs.fetch_sub(1, std::memory_order_release);
        outstanding.fetch_sub(1, std::memory_order_release);
        state->release();
    }
};

JobScheduler::Ticket::Ticket(State* state) noexcept : state_(state) {}
JobScheduler::Ticket::Ticket(const Ticket& other) noexcept : state_(other.state_) {
    if (state_)
        state_->retain();
}
JobScheduler::Ticket& JobScheduler::Ticket::operator=(const Ticket& other) noexcept {
    if (other.state_)
        other.state_->retain();
    if (state_)
        state_->release();
    state_ = other.state_;
    return *this;
}
JobScheduler::Ticket::Ticket(Ticket&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}
JobScheduler::Ticket& JobScheduler::Ticket::operator=(Ticket&& other) noexcept {
    if (this != &other) {
        if (state_)
            state_->release();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}
JobScheduler::Ticket::~Ticket() {
    if (state_)
        state_->release();
}

JobScheduler::JobScheduler() : impl_(new Impl) {}
JobScheduler::~JobScheduler() {
    // The host must stop concurrent submit/help calls before destruction. Drain any
    // accepted jobs locally; do not wait for work owned by another thread.
    while (helpOne()) {
    }
    assert(!hasOutstanding());
    delete impl_;
}

JobScheduler::Ticket JobScheduler::submit(const TaskGraph& source) {
    return submit(source.prepare());
}

JobScheduler::Ticket JobScheduler::submit(const PreparedTaskGraph& source) {
    Ticket ticket(new State);
    auto* state = ticket.state_;
    state->graph = source.graph_;
    state->topology = source.topology_;
    if (!source.valid()) {
        state->compileError = source.error();
        markFailure(state->firstFailure, 0);
        return ticket;
    }
    state->taskCount = state->graph->tasks().size();
    if (state->taskCount == 0)
        return ticket;
    state->pending = std::make_unique<std::atomic<std::size_t>[]>(state->taskCount);
    for (std::size_t i = 0; i < state->taskCount; ++i)
        state->pending[i].store(0, std::memory_order_relaxed);
    for (const auto& following : state->topology->successors)
        for (const auto id : following)
            state->pending[id].fetch_add(1, std::memory_order_relaxed);
    std::vector<TaskId> roots;
    for (const auto id : state->topology->order)
        if (state->pending[id].load(std::memory_order_relaxed) == 0)
            roots.push_back(id);
    for (const auto id : roots)
        impl_->enqueue(state, id);
    return ticket;
}

JobStatus JobScheduler::poll(const Ticket& ticket) const {
    JobStatus status;
    if (!ticket.state_) {
        status.complete = true;
        status.error = "Invalid job ticket";
        return status;
    }
    auto* state = ticket.state_;
    status.complete = state->complete();
    status.completed = state->completed.load(std::memory_order_relaxed);
    if (!status.complete)
        return status;
    const auto failedTask = state->firstFailure.load(std::memory_order_relaxed);
    status.success = failedTask == std::numeric_limits<std::size_t>::max();
    if (!state->compileError.empty())
        status.error = state->compileError;
    else if (state->queueFailure.load(std::memory_order_relaxed))
        status.error = "Task queue allocation failed";
    else if (!status.success && failedTask < state->taskCount)
        status.error = "Task failed: " + state->graph->tasks()[failedTask].name;
    return status;
}

bool JobScheduler::complete(const Ticket& ticket) const noexcept {
    return !ticket.state_ || ticket.state_->complete();
}

bool JobScheduler::helpOne() {
    Impl::QueuedJob job;
    {
        typename LfrcReclaimer::region_guard guard;
        (void)guard;
        if (!impl_->ready.try_pop(job))
            return false;
    }
    impl_->execute(std::move(job));
    return true;
}

bool JobScheduler::hasOutstanding() const noexcept {
    return impl_->outstanding.load(std::memory_order_acquire) != 0;
}

bool JobScheduler::inTask() noexcept {
    return taskDepth != 0;
}

static_assert(std::atomic<std::size_t>::is_always_lock_free,
              "scheduler counters must use lock-free atomics");
static_assert(std::atomic<bool>::is_always_lock_free, "scheduler flags must use lock-free atomics");

} // namespace engine::jobs
