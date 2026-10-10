#include "engine/jobs/task_graph.hpp"
#include <algorithm>
#include <deque>

namespace engine::jobs {
TaskId TaskGraph::add(std::string name, std::function<bool()> work,
                      std::vector<TaskId> prerequisites) {
    tasks_.push_back({std::move(name), std::move(work), std::move(prerequisites)});
    return tasks_.size() - 1;
}
bool TaskGraph::depends(TaskId task, TaskId prerequisite) {
    if (task >= tasks_.size() || prerequisite >= tasks_.size())
        return false;
    tasks_[task].prerequisites.push_back(prerequisite);
    return true;
}
bool TaskGraph::compile(std::vector<TaskId>& order, std::vector<std::vector<TaskId>>& successors,
                        std::string& error) const {
    order.clear();
    error.clear();
    successors.assign(tasks_.size(), {});
    std::vector<std::size_t> pending(tasks_.size());
    for (TaskId i = 0; i < tasks_.size(); ++i) {
        if (!tasks_[i].work) {
            error = "Task has no work: " + tasks_[i].name;
            return false;
        }
        auto deps = tasks_[i].prerequisites;
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        for (auto p : deps) {
            if (p >= tasks_.size()) {
                error = "Invalid prerequisite: " + tasks_[i].name;
                return false;
            }
            successors[p].push_back(i);
            ++pending[i];
        }
    }
    std::deque<TaskId> ready;
    for (TaskId i = 0; i < tasks_.size(); ++i)
        if (!pending[i])
            ready.push_back(i);
    while (!ready.empty()) {
        auto i = ready.front();
        ready.pop_front();
        order.push_back(i);
        for (auto next : successors[i])
            if (!--pending[next])
                ready.push_back(next);
    }
    if (order.size() != tasks_.size()) {
        error = "Task dependency cycle";
        return false;
    }
    return true;
}

PreparedTaskGraph TaskGraph::prepare() const {
    PreparedTaskGraph prepared;
    auto graph = std::make_shared<TaskGraph>(*this);
    auto topology = std::make_shared<PreparedTaskGraph::Topology>();
    graph->compile(topology->order, topology->successors, topology->error);
    prepared.graph_ = std::move(graph);
    prepared.topology_ = std::move(topology);
    return prepared;
}

bool PreparedTaskGraph::valid() const noexcept {
    return graph_ && topology_ && topology_->error.empty();
}

const std::string& PreparedTaskGraph::error() const noexcept {
    static const std::string invalid = "Invalid prepared task graph";
    return topology_ ? topology_->error : invalid;
}

std::size_t PreparedTaskGraph::size() const noexcept {
    return graph_ ? graph_->tasks().size() : 0;
}
} // namespace engine::jobs
