#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine::jobs {
using TaskId = std::size_t;
class TaskGraph;

class PreparedTaskGraph {
  public:
    bool valid() const noexcept;
    const std::string& error() const noexcept;
    std::size_t size() const noexcept;

  private:
    friend class TaskGraph;
    friend class JobScheduler;
    struct Topology {
        std::vector<TaskId> order;
        std::vector<std::vector<TaskId>> successors;
        std::string error;
    };
    std::shared_ptr<const TaskGraph> graph_;
    std::shared_ptr<const Topology> topology_;
};

class TaskGraph {
  public:
    struct Task {
        std::string name;
        std::function<bool()> work;
        std::vector<TaskId> prerequisites;
    };
    TaskId add(std::string name, std::function<bool()> work,
               std::vector<TaskId> prerequisites = {});
    bool depends(TaskId task, TaskId prerequisite);
    const std::vector<Task>& tasks() const {
        return tasks_;
    }
    bool compile(std::vector<TaskId>& order, std::vector<std::vector<TaskId>>& successors,
                 std::string& error) const;
    PreparedTaskGraph prepare() const;

  private:
    std::vector<Task> tasks_;
};
} // namespace engine::jobs
