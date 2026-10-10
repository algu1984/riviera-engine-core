#pragma once

#include "engine/jobs/task_graph.hpp"
#include <cstddef>
#include <string>

namespace engine::jobs {

struct JobStatus {
    bool complete = false;
    bool success = false;
    std::size_t completed = 0;
    std::string error;
};

class JobScheduler {
    struct State;

  public:
    class Ticket {
      public:
        Ticket() = default;
        Ticket(const Ticket& other) noexcept;
        Ticket& operator=(const Ticket& other) noexcept;
        Ticket(Ticket&& other) noexcept;
        Ticket& operator=(Ticket&& other) noexcept;
        ~Ticket();

        explicit operator bool() const noexcept {
            return state_ != nullptr;
        }

      private:
        friend class JobScheduler;
        explicit Ticket(State* state) noexcept;
        State* state_ = nullptr;
    };

    JobScheduler();
    // Destruction drains queued jobs locally. Callers must stop concurrent submit/helpOne
    // calls first; engine hosts drain and join workers before destroying the scheduler.
    ~JobScheduler();
    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    // Copies and compiles the graph. The caller must not mutate the source graph
    // concurrently with this call. Submitted work owns its graph until completion;
    // referenced data captured by callbacks must remain valid until the ticket completes.
    Ticket submit(const TaskGraph& graph);
    // Reuses an immutable graph and its compiled topology across submissions.
    Ticket submit(const PreparedTaskGraph& graph);
    JobStatus poll(const Ticket& ticket) const;
    bool complete(const Ticket& ticket) const noexcept;

    // Executes at most one ready task. Work can belong to any submitted graph.
    bool helpOne();
    bool hasOutstanding() const noexcept;

    // True while the current thread is running a task from any JobScheduler.
    static bool inTask() noexcept;

  private:
    struct Impl;
    Impl* impl_;
};

} // namespace engine::jobs
