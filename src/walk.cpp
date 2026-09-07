#include "same/walk.hpp"
#include "same/config.hpp"
#include "same/detail/windows_path.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
namespace same {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
/// 路径编码不受系统区域设置影响。 / Path encoding independent of system locale.
std::string key_of(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
/// 墙钟毫秒差。 / Wall-clock milliseconds.
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
/// 枚举缓存属性；不把目录缓存时间当成文件版本。 / Enumeration attributes, never used as a version
/// stamp.
struct Entry {
    fs::path path;
    bool directory, regular, reparse;
};
/** 单目录游标，独占原生枚举句柄。 / Single-directory cursor owning its enumeration handle. */
class Cursor {
public:
    /// Windows 直接复用 FindFirstFileEx 属性，避免每项再查路径。 / Reuse Windows enumeration
    /// attributes without a per-entry path query.
    explicit Cursor(const fs::path& path) : directory_(path) {
#ifdef _WIN32
        const auto pattern = detail::windows_path(path / L"*");
        handle_ = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data_, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND)
                return;
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "enumerate directory: " + key_of(path));
        }
        valid_ = true;
        try {
            skip_dots();
        } catch (...) {
            FindClose(handle_);
            throw;
        }
#else
        iterator_ = fs::directory_iterator(path);
#endif
    }
    /// vector 扩容仅移动句柄，不复制所有权。 / Vector growth moves, never copies, ownership.
    Cursor(Cursor&& other) noexcept
        : directory_(std::move(other.directory_))
#ifdef _WIN32
          ,
          handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)), data_(other.data_),
          valid_(other.valid_)
#else
          ,
          iterator_(std::move(other.iterator_))
#endif
    {
    }
    Cursor(const Cursor&) = delete;
    /// 关闭枚举句柄。 / Close the enumeration handle.
    ~Cursor() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE)
            FindClose(handle_);
#endif
    }
    /// 已耗尽目录不再产生条目。 / Exhausted directory produces no entries.
    bool empty() const {
#ifdef _WIN32
        return !valid_;
#else
        return iterator_ == fs::directory_iterator{};
#endif
    }
    /// 获取缓存目录项类型，POSIX 链接仍只看 symlink_status。 / Read cached entry kind; POSIX uses
    /// symlink_status only.
    Entry entry() const {
#ifdef _WIN32
        const auto attributes = data_.dwFileAttributes;
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        return {directory_ / data_.cFileName, directory, !directory,
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0};
#else
        const auto status = iterator_->symlink_status();
        return {iterator_->path(), fs::is_directory(status), fs::is_regular_file(status),
                fs::is_symlink(status)};
#endif
    }
    /// 读取错误必须与正常目录结束区分。 / Distinguish read failures from normal directory EOF.
    void advance() {
#ifdef _WIN32
        step();
        skip_dots();
#else
        ++iterator_;
#endif
    }

private:
    /// 路径只用于拼接缓存键与错误诊断。 / Directory path for keys and diagnostics.
    fs::path directory_;
#ifdef _WIN32
    /// 唯一枚举所有权与缓存项。 / Exclusive enumeration ownership and cached item.
    HANDLE handle_{INVALID_HANDLE_VALUE};
    WIN32_FIND_DATAW data_{};
    bool valid_{false};
    /// 推进一个原生条目。 / Advance one native entry.
    void step() {
        if (FindNextFileW(handle_, &data_))
            return;
        const auto error = GetLastError();
        valid_ = false;
        if (error != ERROR_NO_MORE_FILES)
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "enumerate directory: " + key_of(directory_));
    }
    /// 原生 API 返回的点项不可下降。 / Never descend into native dot entries.
    void skip_dots() {
        while (valid_ && (std::wstring_view(data_.cFileName) == L"." ||
                          std::wstring_view(data_.cFileName) == L".."))
            step();
    }
#else
    /// 标准库游标独占于单工作线程。 / STL cursor confined to one worker.
    fs::directory_iterator iterator_;
#endif
};

} // namespace
struct ParallelWalk::Impl {
    /// 尚未展开的目录或尚未打开的文件。 / An unexpanded directory or unopened file.
    struct Task {
        /// 原生绝对路径。 / Native absolute path.
        fs::path path;
        /// 已经过滤后的类型。 / Already-filtered entry type.
        bool directory;
    };
    /// 不可变根与规则可安全共享。 / Immutable root and rules are safely shared.
    fs::path root;
    Ignore ignore;
    /// 两个队列分别有界，不限制 DFS 深度。 / Both queues bounded; DFS depth is not capped.
    std::size_t capacity;
    /// 浅扫描在目录分类时直接截断，不会打开子目录。 / Shallow scans never open child directories.
    bool recursive;
    /// 保护队列、计数、错误及统计。 / Protect queues, counters, failure and statistics.
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> tasks;
    std::deque<WalkEntry> results;
    /// 正在执行的顶层任务数；本地 DFS 仍属于该任务。 / Active top-level jobs, including local DFS.
    std::size_t active{};
    /// 停止标志也供无锁热循环查询。 / Stop flag also read by lock-free hot loops.
    std::atomic<bool> stopped{false};
    std::exception_ptr failure;
    WalkStats totals;
    /// 必须最后构造；析构显式先 join。 / Construct last; explicitly join before destroying state.
    std::vector<std::thread> threads;

    /// 启动失败同样停止已经启动的线程。 / Stop started threads even if startup fails.
    Impl(const fs::path& path, std::size_t workers, std::size_t bound, bool descend)
        : root(fs::absolute(path).lexically_normal()), ignore(root), capacity(bound),
          recursive(descend) {
        if (!workers || !capacity)
            throw std::invalid_argument("walk workers and capacity must be nonzero");
        tasks.push_back({root, true});
        totals.task_peak = 1;
        try {
            for (std::size_t i = 0; i < workers; ++i)
                threads.emplace_back([this] { run(); });
        } catch (...) {
            stop();
            throw;
        }
    }
    /// 不持锁 join，防止退出路径死锁。 / Never hold the mutex while joining.
    void stop() {
        {
            std::lock_guard lock(mutex);
            stopped.store(true);
        }
        changed.notify_all();
        for (auto& thread : threads)
            if (thread.joinable())
                thread.join();
    }
    /// 递归生产者绝不等待任务容量；满时调用方执行。 / Recursive producers never wait for task
    /// capacity.
    bool offer(const Task& task) {
        std::lock_guard lock(mutex);
        if (tasks.size() >= capacity || stopped.load())
            return false;
        tasks.push_back(task);
        totals.task_peak = std::max(totals.task_peak, tasks.size());
        changed.notify_all();
        return true;
    }
    /// 输出背压只依赖外部消费者，不依赖另一个遍历工作线程。 / Output backpressure depends only on
    /// the external consumer.
    void metadata(const fs::path& path, WalkStats& stats) {
        const auto start = Clock::now();
        auto reader = std::make_unique<FileReader>(path);
        WalkEntry entry{key_of(path.lexically_relative(root)), reader->stamp(), std::move(reader)};
        stats.metadata_ms += elapsed(start);
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopped.load() || results.size() < capacity; });
        if (stopped.load())
            return;
        results.push_back(std::move(entry));
        totals.result_peak = std::max(totals.result_peak, results.size());
        changed.notify_all();
    }
    /// 保留原有链接与忽略语义；目录在下降前过滤。 / Preserve links/ignore semantics; filter before
    /// descending.
    std::optional<Task> classify(const Entry& entry) {
        const auto path = entry.path;
        const auto key = key_of(path.lexically_relative(root));
        if (entry.reparse)
            return {};
        if (entry.directory) {
            if (!recursive || ignore.can_prune(key) || fs::equivalent(path, root / ".same"))
                return {};
            return Task{path, true};
        }
        if (entry.regular && !ignore.matches(key, false))
            return Task{path, false};
        return {};
    }
    /// 队列满时显式 DFS 栈仅随深度增长，避免递归 C++ 栈与目录宽度增长。
    /// On saturation, explicit DFS frames grow with depth, not directory width or C++ recursion.
    void execute(const Task& task, WalkStats& stats) {
        if (!task.directory) {
            metadata(task.path, stats);
            return;
        }
        auto start = Clock::now();
        std::vector<Cursor> frames;
        frames.emplace_back(task.path);
        stats.enumerate_ms += elapsed(start);
        while (!frames.empty() && !stopped.load()) {
            start = Clock::now();
            if (frames.back().empty()) {
                frames.pop_back();
                stats.enumerate_ms += elapsed(start);
                continue;
            }
            const auto child = classify(frames.back().entry());
            frames.back().advance();
            stats.enumerate_ms += elapsed(start);
            if (!child || offer(*child))
                continue;
            if (!child->directory) {
                metadata(child->path, stats);
                continue;
            }
            start = Clock::now();
            frames.emplace_back(child->path);
            stats.enumerate_ms += elapsed(start);
        }
    }
    /// 空队列且没有执行者才是完成；空队列本身不是终止证明。 / Empty queue plus zero active jobs
    /// proves completion.
    void run() noexcept {
        try {
            for (;;) {
                Task task;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopped.load() || !tasks.empty() || !active; });
                    if (stopped.load() || tasks.empty())
                        return;
                    task = std::move(tasks.front());
                    tasks.pop_front();
                    ++active;
                }
                WalkStats stats;
                execute(task, stats);
                {
                    std::lock_guard lock(mutex);
                    totals.enumerate_ms += stats.enumerate_ms;
                    totals.metadata_ms += stats.metadata_ms;
                    --active;
                }
                changed.notify_all();
            }
        } catch (...) {
            {
                std::lock_guard lock(mutex);
                if (!failure)
                    failure = std::current_exception();
                stopped.store(true);
            }
            changed.notify_all();
        }
    }
};
ParallelWalk::ParallelWalk(const fs::path& root, std::size_t workers, std::size_t capacity,
                           bool recursive)
    : impl_(std::make_unique<Impl>(root, workers, capacity, recursive)) {}
ParallelWalk::~ParallelWalk() {
    impl_->stop();
}
std::optional<WalkEntry> ParallelWalk::next() {
    std::unique_lock lock(impl_->mutex);
    impl_->changed.wait(lock, [&] {
        return impl_->failure || !impl_->results.empty() ||
               (impl_->tasks.empty() && !impl_->active);
    });
    if (impl_->failure)
        std::rethrow_exception(impl_->failure);
    if (impl_->results.empty())
        return {};
    auto entry = std::move(impl_->results.front());
    impl_->results.pop_front();
    impl_->changed.notify_all();
    return entry;
}
WalkStats ParallelWalk::stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->totals;
}
} // namespace same
