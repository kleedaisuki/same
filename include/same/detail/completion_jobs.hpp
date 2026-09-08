#pragma once
#include "same/resources.hpp"
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace same::detail {
/**
 * @brief 按完成顺序收取有界任务。 / Collect bounded tasks in completion order.
 *
 * 发布结果不等待空间；共享状态覆盖异常退出后的在途任务。
 * Publishers never wait for space; shared state outlives in-flight tasks on failure.
 *
 * 单个协调线程调用 submit/next/pending；捕获对象须自行管理生命周期。
 * One coordinator calls submit/next/pending; captures must manage their own lifetimes.
 *
 * @code
 * CompletionJobs<int> jobs(resources, 2);
 * jobs.submit([](Worker&) { return 42; });
 * auto value = jobs.next();
 * @endcode
 */
template <class Result> class CompletionJobs {
    static_assert(std::is_nothrow_move_constructible_v<Result> &&
                      std::is_nothrow_move_assignable_v<Result>,
                  "Completion publication requires nonthrowing result moves");
    /// 值与异常互斥；空值只存在于尚未发布的环槽中。
    /// A value or exception; empty slots have not been published.
    struct Outcome {
        /// 已完成任务的结果。 / Completed operation result.
        std::optional<Result> value;
        /// 工作线程异常，由协调线程重新抛出。 / Worker failure rethrown by the coordinator.
        std::exception_ptr error;
    };
    /// 预分配完成环，任务不在发布时分配队列节点。
    /// Preallocated completion ring avoids allocating queue nodes in publishers.
    struct State {
        /// 固定容量的结果存储。 / Fixed-capacity result storage.
        std::vector<Outcome> ring;
        /// 保护环及游标。 / Protect the ring and cursors.
        std::mutex mutex;
        /// 通知协调线程有结果。 / Signal a completed result.
        std::condition_variable ready;
        /// 读写游标及已发布结果数。 / Read/write positions and published result count.
        std::size_t read{}, write{}, count{};
        /// 所有任务启动前预分配。 / Allocate before starting any task.
        explicit State(std::size_t capacity) : ring(capacity) {}
    };

public:
    /// Resources 必须覆盖提交调用；任务自身保留完成环的所有权。
    /// Resources must outlive submissions; tasks retain ownership of the completion ring.
    CompletionJobs(Resources& resources, std::size_t capacity)
        : resources_(resources), state_(std::make_shared<State>(capacity)) {
        if (!capacity)
            throw std::invalid_argument("completion capacity must be nonzero");
    }
    /// 协调状态不可复制；单个协调线程独占。 / One coordinator exclusively owns admission state.
    CompletionJobs(const CompletionJobs&) = delete;
    CompletionJobs& operator=(const CompletionJobs&) = delete;
    /// 达到容量前提交；调用方先 drain，再接收更多任务。
    /// Submit below capacity; callers drain before admitting further work.
    template <class F> void submit(F&& operation, bool prefer_gpu = false) {
        submit_impl(std::forward<F>(operation), prefer_gpu, std::nullopt);
    }
    /// 哈希任务携带可窃取的 GPU 资格，不固定到单一工作线程。
    /// Hash work carries a stealable GPU eligibility hint, not a fixed worker assignment.
    template <class F> void submit_hash(F&& operation, bool gpu_eligible) {
        submit_hash(std::forward<F>(operation),
                    gpu_eligible ? HashRoute::gpu_preferred : HashRoute::cpu_only);
    }
    /// 显式区分不可卸载与可忙时互补的载荷。 / Distinguish CPU-only work from saturation spill.
    template <class F> void submit_hash(F&& operation, HashRoute route) {
        submit_impl(std::forward<F>(operation), false, route);
    }

private:
    /// 两类接收共用完成发布和容量契约，仅资源调度入口不同。
    /// Both admission routes share publication and capacity contracts, differing only in
    /// scheduling.
    template <class F>
    void submit_impl(F&& operation, bool pinned, std::optional<HashRoute> route) {
        if (pending_ == state_->ring.size())
            throw std::logic_error("completion admission capacity exceeded");
        auto publish = [state = state_,
                        operation = std::forward<F>(operation)](Worker& worker) mutable {
            Outcome result;
            try {
                result.value = operation(worker);
            } catch (...) {
                result.error = std::current_exception();
            }
            {
                std::lock_guard lock(state->mutex);
                state->ring[state->write] = std::move(result);
                state->write = (state->write + 1) % state->ring.size();
                ++state->count;
            }
            state->ready.notify_one();
        };
        if (route)
            resources_.submit_hash(std::move(publish), *route);
        else
            resources_.submit(std::move(publish), pinned);
        ++pending_;
    }

public:
    /// 等待任意任务完成，不按提交顺序阻塞。 / Wait for any completion, not submission order.
    Result next() {
        if (!pending_)
            throw std::logic_error("no pending completion jobs");
        std::unique_lock lock(state_->mutex);
        state_->ready.wait(lock, [&] { return state_->count != 0; });
        auto result = std::move(state_->ring[state_->read]);
        state_->ring[state_->read] = {};
        state_->read = (state_->read + 1) % state_->ring.size();
        --state_->count;
        --pending_;
        lock.unlock();
        if (result.error)
            std::rethrow_exception(result.error);
        return std::move(*result.value);
    }
    /// 已提交但尚未收取的任务数，含执行和完成态。
    /// Submitted but unconsumed tasks, including running and completed work.
    std::size_t pending() const {
        return pending_;
    }

private:
    /// 非拥有的工作线程池引用。 / Borrowed worker pool.
    Resources& resources_;
    /// 任务共享的完成队列生命周期。 / Completion queue lifetime shared with tasks.
    std::shared_ptr<State> state_;
    /// 仅由协调线程访问的接收计数。 / Coordinator-only admission count.
    std::size_t pending_{};
};
} // namespace same::detail
