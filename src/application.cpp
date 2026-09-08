#include "same/application.hpp"
#include "same/detail/completion_jobs.hpp"
#include "same/files.hpp"
#include "same/resources.hpp"
#include "same/run_lock.hpp"
#include "same/store.hpp"
#include "same/terminal.hpp"
#include "same/walk.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <future>
#include <iomanip>
#include <locale>
#include <numeric>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace same {
namespace {
namespace fs = std::filesystem;
/// Monotonic wall clock for overlapping pipeline measurements.
/// 用于重叠流水线计量的单调墙钟。
using Clock = std::chrono::steady_clock;
/// Convert one measured interval to milliseconds. 将一个测量区间转换为毫秒。
double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
/// Persist paths as generic UTF-8 bytes, independent of native separators.
/// 以通用 UTF-8 字节持久化路径，不依赖本机分隔符。
std::string path_key(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
/// Decode an internal root-relative database key without locale conversion.
/// 无需本地编码转换，将内部相对路径键还原为本机路径。
fs::path native_path(const fs::path& root, const std::string& key) {
    return root /
           fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(key.data()), key.size()));
}
/// Check the open object and its current path binding; this is not an atomic snapshot.
/// 同时检查已打开对象及当前路径绑定；这不等同于原子快照。
void unchanged(const fs::path& path, FileReader& reader, const FileStamp& expected) {
    if (reader.stamp() != expected || stamp_path(path) != expected)
        throw std::runtime_error("file changed while processing: " + path_key(path));
}
/// Coalesce short reads until the buffer is full or EOF is reached.
/// 合并短读，直到缓冲区填满或遇到文件末尾。
std::size_t read_block(FileReader& reader, std::span<std::byte> buffer, std::uint64_t& bytes) {
    std::size_t count = 0;
    while (count < buffer.size()) {
        const auto read = reader.read(buffer.subspan(count));
        if (!read)
            break;
        count += read;
        bytes += read;
    }
    return count;
}
/// Hash from a fresh handle; reject size/stamp changes before publishing the digest.
/// 从新句柄计算哈希；发布摘要前拒绝大小或文件戳变化。
FileRecord hash_file(const fs::path& root, FileRecord record, Worker& worker,
                     std::size_t gpu_min_bytes, std::unique_ptr<FileReader> opened = {}) {
    const auto path = native_path(root, record.path);
    // 元数据句柄仅首次尝试复用；重试必须从新句柄的起点读取。
    // Reuse the metadata handle only on the first attempt; retries reopen at offset zero.
    auto owned = opened ? std::move(opened) : std::make_unique<FileReader>(path);
    auto& reader = *owned;
    unchanged(path, reader, record.stamp);
    const bool small = record.stamp.size < std::max(gpu_min_bytes, worker.gpu_floor);
    if (small)
        ++worker.cpu_routed_hashes;
    auto& selected = small ? worker.cpu_compute : worker.compute;
    if (selected->name() == "cuda")
        ++worker.gpu_hashes;
    else
        ++worker.cpu_hashes;
    auto hasher = selected->hasher();
    std::uint64_t total = 0;
    for (;;) {
        const auto count = reader.read(worker.first);
        worker.hash_bytes += count;
        if (!count)
            break;
        if (count > record.stamp.size - total)
            throw std::runtime_error("file grew while hashing: " + record.path);
        hasher->update(std::span(worker.first).first(count));
        total += count;
    }
    if (total != record.stamp.size)
        throw std::runtime_error("file shrank while hashing: " + record.path);
    record.digest = hasher->finish();
    unchanged(path, reader, record.stamp);
    return record;
}
/// Digest equality only selects candidates: establish equality from actual bytes.
/// 摘要相等仅用于筛选候选：真正相等必须逐字节验证。
bool equal_files(const fs::path& root, const FileRecord& a, const FileRecord& b, Worker& worker) {
    const auto path_a = native_path(root, a.path), path_b = native_path(root, b.path);
    FileReader first(path_a), second(path_b);
    unchanged(path_a, first, a.stamp);
    unchanged(path_b, second, b.stamp);
    bool equal = a.stamp.size == b.stamp.size;
    auto left = a.stamp.size;
    while (equal && left) {
        const auto count =
            static_cast<std::size_t>(std::min<std::uint64_t>(left, worker.first.size()));
        auto x = std::span(worker.first).first(count), y = std::span(worker.second).first(count);
        if (read_block(first, x, worker.compare_bytes) != count ||
            read_block(second, y, worker.compare_bytes) != count)
            throw std::runtime_error("file truncated during comparison: " + a.path + " / " +
                                     b.path);
        equal = worker.compute->equal(x, y);
        left -= count;
    }
    unchanged(path_a, first, a.stamp);
    unchanged(path_b, second, b.stamp);
    return equal;
}
/// Escape controls and delimiters so a filename cannot forge another output row.
/// 转义控制字符和分隔符，防止文件名伪造额外输出行。
void quoted_path(std::ostream& out, std::string_view path) {
    constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (unsigned char c : path) {
        if (c == '"' || c == '\\')
            out << '\\' << static_cast<char>(c);
        else if (c < 32 || c == 127)
            out << "\\u00" << digits[c >> 4] << digits[c & 15];
        else
            out << static_cast<char>(c);
    }
    out << '"';
}
/// Reject linked state paths before opening SQLite; concurrent hostile replacement
/// still requires filesystem-level isolation rather than these preflight checks.
/// 打开 SQLite 前拒绝链接状态路径；恶意并发替换仍需文件系统隔离，不能仅靠预检查。
void prepare_state(const fs::path& root) {
    const auto state = root / ".same";
    const auto status = fs::symlink_status(state);
    if (fs::exists(status)) {
        if (!fs::is_directory(status) || is_reparse_point(state))
            throw std::runtime_error(".same must be a real directory, not a link");
    } else
        fs::create_directory(state);
    for (const auto* name :
         {"state.db", "state.db-wal", "state.db-shm", "state.db-journal", "run.lock"}) {
        const auto path = state / name;
        const auto entry = fs::symlink_status(path);
        if (fs::exists(entry) && (!fs::is_regular_file(entry) || is_reparse_point(path)))
            throw std::runtime_error("state path must be a regular non-link file: " +
                                     path_key(path));
    }
}
/// Main-thread-only diagnostic totals. 仅由主线程维护的诊断统计。
struct Counters {
    /// Eligible regular files visited. 扫描到的合格普通文件数。
    std::size_t scanned = 0;
    /// Main state.db file length after scan commit; excludes journals and temporary storage.
    /// 扫描提交后 state.db 主文件长度；不含日志和临时存储。
    std::uintmax_t database_bytes = 0;
    /// Logical sizes, not physical disk traffic. 逻辑大小，并非物理磁盘流量。
    std::uint64_t scanned_bytes = 0, cached_bytes = 0;
    /// Files submitted for hashing. 已提交哈希计算的文件数。
    std::size_t hashed = 0;
    /// Main-thread hash submission/get intervals, including dispatch overhead.
    /// 主线程提交及获取哈希结果的区间，包含调度开销。
    double hash_wait_ms = 0;
    /// Sum of worker hash-job wall times, including reads and CPU fallback retries.
    /// 工作线程哈希任务墙钟耗时之和，包含读取及 CPU 回退重试。
    double hash_work_ms = 0;
    /// 协调线程等待元数据结果的时间，可能与哈希重叠。
    /// Coordinator wait for metadata results, potentially overlapping hashing.
    double walk_wait_ms = 0;
    /// 目录/元数据工作线程累计耗时，不得与关键路径相加。
    /// Summed directory/metadata worker time, not additive to critical-path duration.
    double enumerate_work_ms = 0, metadata_work_ms = 0;
    /// 协调线程数据库查询、更新及提交耗时。 / Coordinator database query/update/commit time.
    double database_work_ms = 0;
    /// 遍历任务和结果队列的峰值。 / Peak traversal task and result queue lengths.
    std::size_t walk_task_peak = 0, walk_result_peak = 0;
    /// Records reused after matching stamps. 文件戳匹配后复用的记录数。
    std::size_t cached = 0;
    /// Emitted duplicate equivalence classes. 输出的重复文件等价类数量。
    std::size_t groups = 0;
    /// Emitted members, including representatives. 输出成员数，包含代表文件。
    std::size_t matches = 0;
};
/// Hash result and job-local timing, transferred together via a future.
/// 哈希结果及任务本地耗时，通过同一个 future 传递。
struct HashResult {
    /// Successfully verified digest and metadata. 成功验证的摘要与元数据。
    FileRecord record;
    /// Worker wall time; queue residence excluded. 工作线程墙钟时间，不含排队。
    double work_ms;
};
/// 待哈希记录与未读句柄；仅协调线程转移所有权。
/// Pending hash metadata and unread handle, moved only by the coordinator.
struct HashInput {
    /// 待处理的缓存记录。 / Pending cache record.
    FileRecord record;
    /// 从元数据阶段移交的未读取句柄。 / Unread handle from metadata work.
    std::unique_ptr<FileReader> reader;
};
/// 后台初始化与 CPU 流水线重叠，只保留一个最大的候选句柄。
/// Overlap background initialization with CPU work, retaining only the largest candidate.
class AutoHashStartup {
public:
    /// 借用扫描作用域外的服务；future 析构在服务销毁前等待初始化。
    /// Borrow scan-external services; future destruction joins before those services die.
    AutoHashStartup(const Config& config, Resources& resources)
        : config_(config), resources_(resources),
          floor_(std::max<std::size_t>(1, config.gpu_min_bytes)), ready_(config.backend != "auto") {
    }

    /// 不迁移已排队任务；初始化期间以更大候选替换唯一保留项。
    /// Never migrate queued work; replace the sole retained candidate with larger input.
    template <class Submit> void accept(HashInput input, Submit& submit) {
        if (startup_.valid() &&
            startup_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            finish(submit);
        if (ready_ || input.record.stamp.size < floor_) {
            send(std::move(input), submit);
            return;
        }
        if (!startup_.valid()) {
            const auto bytes = input.record.stamp.size;
            startup_ = std::async(std::launch::async,
                                  [this, bytes] { resources_.prepare_auto(config_, bytes, 1); });
            held_ = std::move(input);
            return;
        }
        if (input.record.stamp.size > held_->record.stamp.size)
            std::swap(input, *held_);
        send(std::move(input), submit);
    }

    /// EOF 等待时已提交的 CPU 任务继续运行；get 发布探测证据后再读取。
    /// At EOF submitted CPU work keeps running; get publishes evidence before any read.
    template <class Submit> void finish(Submit& submit) {
        if (!startup_.valid())
            return;
        startup_.get();
        ready_ = true;
        auto input = std::move(*held_);
        held_.reset();
        send(std::move(input), submit);
    }

private:
    /// 未发布证据时只按载荷底线分类，避免与后台初始化的数据竞争。
    /// Before evidence publication classify by the size floor only, avoiding startup races.
    template <class Submit> void send(HashInput input, Submit& submit) {
        auto route = input.record.stamp.size < floor_ ? detail::HashRoute::cpu_only
                                                      : detail::HashRoute::cpu_preferred;
        if (ready_)
            route =
                detail::classify_hash(input.record.stamp.size, floor_, resources_.gpu_block_bytes(),
                                      resources_.dispatch_evidence());
        // 没有合成校准时以研究阈值冷启动，真实任务模型在出队时修正偏好。
        // Without synthetic calibration start from the research threshold; real-task models
        // refine the preference at dequeue. Explicit backends and the eligibility floor remain.
        if (ready_ && config_.backend == "auto") {
            const auto& evidence = resources_.dispatch_evidence();
            const bool initial =
                !config_.pgo || (!evidence.block_complete && !evidence.stream_complete);
            if (initial && input.record.stamp.size >= floor_ &&
                input.record.stamp.size >= 64ULL * 1024 * 1024)
                route = detail::HashRoute::gpu_preferred;
        }
        submit(std::move(input), route);
    }
    /// 配置及资源由 run 持有，覆盖后台任务。 / Run owns services beyond background work.
    const Config& config_;
    Resources& resources_;
    /// 不卸载的小载荷上界。 / Floor below which payloads are never offloaded.
    std::size_t floor_;
    /// 仅协调线程访问；为真意味着初始化证据可读。 / Coordinator-only publication state.
    bool ready_;
    /// 初始化期间至多一个尚未提交的句柄。 / At most one unsubmitted startup handle.
    std::optional<HashInput> held_;
    /// 最后声明以便异常展开时首先 join，保护捕获的 this 及借用服务。
    /// Declared last to join first on unwinding, protecting captured this and borrowed services.
    std::future<void> startup_;
};
/// Stream the tree into bounded worker jobs; keep every SQLite mutation on this thread.
/// 流式遍历目录并提交有界任务；所有 SQLite 修改留在当前线程。
void scan(const fs::path& root, const Config& config, Store& store, Resources& resources,
          Counters& counters, bool recursive) {
    detail::CompletionJobs<HashResult> pending(resources, config.queue_capacity);
    auto save = [&](const FileRecord& record) {
        const auto start = Clock::now();
        store.save(record);
        counters.database_work_ms += milliseconds(start, Clock::now());
    };
    auto drain = [&] {
        const auto start = Clock::now();
        auto result = pending.next();
        counters.hash_wait_ms += milliseconds(start, Clock::now());
        counters.hash_work_ms += result.work_ms;
        save(result.record);
    };
    auto submit = [&](HashInput input, detail::HashRoute route) {
        const auto submitted = Clock::now();
        const auto bytes = input.record.stamp.size;
        auto operation = [root, gpu_min_bytes = config.gpu_min_bytes,
                          record = std::move(input.record),
                          opened = std::move(input.reader)](Worker& worker) mutable {
            // 每64个小任务抽样；合格任务全采样，关闭时不修改采样状态。
            // Sample every 64th small task and every eligible task; disabled leaves no sample
            // state.
            const bool sample = worker.profile_enabled && (record.stamp.size >= gpu_min_bytes ||
                                                           (++worker.profile_sequence & 63) == 0);
            const auto cpu_before = sample ? worker.cpu_hashes : 0;
            const auto gpu_before = sample ? worker.gpu_hashes : 0;
            const auto start = Clock::now();
            auto hashed = worker.execute(
                [&] { return hash_file(root, record, worker, gpu_min_bytes, std::move(opened)); });
            const auto elapsed = milliseconds(start, Clock::now());
            // 复用已有计时；仅完整成功且没有重试的实际后端样本进入模型。
            // Reuse existing timing; publish only successful, single-attempt actual-backend
            // samples.
            if (sample && worker.cpu_hashes - cpu_before + worker.gpu_hashes - gpu_before == 1)
                worker.sample = {hashed.stamp.size, elapsed, worker.gpu_hashes != gpu_before, true};
            return HashResult{std::move(hashed), elapsed};
        };
        // 只提交偏好；领取时在同一锁下决定真实设备忙闲，不固定首个大文件。
        // Submit a preference; resolve live device occupancy under the queue lock at dequeue.
        pending.submit_hash(std::move(operation), route, bytes);
        counters.hash_wait_ms += milliseconds(submitted, Clock::now());
        if (pending.pending() >= config.queue_capacity)
            drain();
    };
    AutoHashStartup startup(config, resources);
    store.begin_scan();
    ParallelWalk walk(root, config.metadata_workers, config.queue_capacity, recursive);
    for (;;) {
        const auto waited = Clock::now();
        auto entry = walk.next();
        counters.walk_wait_ms += milliseconds(waited, Clock::now());
        if (!entry)
            break;
        ++counters.scanned;
        FileRecord record{std::move(entry->path), std::move(entry->stamp), {}};
        counters.scanned_bytes += record.stamp.size;
        const auto queried = Clock::now();
        const bool cached = !config.rehash && store.mark_if_unchanged(record.path, record.stamp);
        counters.database_work_ms += milliseconds(queried, Clock::now());
        if (cached) {
            ++counters.cached;
            counters.cached_bytes += record.stamp.size;
            continue;
        }
        ++counters.hashed;
        startup.accept({std::move(record), std::move(entry->reader)}, submit);
    }
    startup.finish(submit);
    while (pending.pending())
        drain();
    resources.wait_idle();
    const auto stats = walk.stats();
    counters.enumerate_work_ms = stats.enumerate_ms;
    counters.metadata_work_ms = stats.metadata_ms;
    counters.walk_task_peak = stats.task_peak;
    counters.walk_result_peak = stats.result_peak;
    const auto committed = Clock::now();
    // 淘汰未访问记录，也隔离从递归扫描切换到浅扫描后留下的子目录缓存。
    // Prune unseen records, including child-directory cache entries after switching to shallow
    // mode.
    store.end_scan();
    counters.database_work_ms += milliseconds(committed, Clock::now());
}
/// Refine each size/digest bucket into byte-equal classes without trusting hash uniqueness.
/// 将每个大小/摘要桶细分为字节相等等价类，不假设哈希绝无碰撞。
class Partition {
public:
    /// Borrow run-scoped services; capacity bounds retained comparison futures.
    /// 借用本轮服务；capacity 限制保留的比较 future 数量。
    Partition(const fs::path& root, Store& store, Resources& resources, std::size_t capacity)
        : root_(root), store_(store), resources_(resources), capacity_(capacity) {}
    /// Input must be contiguous by size/digest; drain the old bucket before switching.
    /// 输入必须按大小/摘要连续分桶；切换前排空旧桶。
    void accept(const FileRecord& record) {
        if (!first_ || first_->stamp.size != record.stamp.size || first_->digest != record.digest) {
            flush();
            store_.clear_representatives();
            first_ = record;
            add_representative(record);
            return;
        }
        pending_.push_back({record, compare(*first_, record)});
        if (pending_.size() >= capacity_)
            drain();
    }
    /// Join all comparisons before representatives are replaced or results consumed.
    /// 替换代表或消费结果前等待所有比较结束。
    void flush() {
        while (!pending_.empty())
            drain();
    }

private:
    /// One candidate paired with its first-representative comparison.
    /// 候选文件及其与首个代表的比较任务。
    struct Pending {
        /// Own metadata until comparison completes. 持有元数据直至比较结束。
        FileRecord record;
        /// Propagate worker failures on the main thread. 在主线程传播工作线程错误。
        std::future<bool> equal;
    };
    /// Capture records by value so a later bucket transition cannot invalidate a job.
    /// 按值捕获记录，防止后续换桶使任务引用失效。
    std::future<bool> compare(const FileRecord& a, const FileRecord& b) {
        return resources_.submit([root = root_, a, b](Worker& worker) {
            return worker.execute([&] { return equal_files(root, a, b, worker); });
        });
    }
    /// Seed a class with itself; singleton classes are filtered by the store on output.
    /// 将代表自身加入新类；单成员类由存储层在输出时过滤。
    void add_representative(const FileRecord& record) {
        store_.add_representative(record);
        store_.add_match(record.path, record.path);
    }
    /// Resolve in input order, serializing rare collision-class creation deterministically.
    /// 按输入顺序归并，确保罕见碰撞类的建立顺序确定。
    void drain() {
        auto pending = std::move(pending_.front());
        pending_.pop_front();
        if (pending.equal.get()) {
            store_.add_match(first_->path, pending.record.path);
            return;
        }
        // Only hash collisions take the ordered secondary-representative path.
        // Normal buckets compare concurrently against their first member.
        // 仅哈希碰撞按序检查其他代表；通常的桶并发比较首个成员。
        bool matched = false;
        store_.visit_representatives([&](const FileRecord& representative) {
            if (representative.path == first_->path ||
                !compare(representative, pending.record).get())
                return true;
            store_.add_match(representative.path, pending.record.path);
            matched = true;
            return false;
        });
        if (!matched)
            add_representative(pending.record);
    }
    /// Root copied into asynchronous comparisons. 复制到异步比较中的根路径。
    fs::path root_;
    /// Main-thread database owner outlives this partitioner. 生命周期更长的主线程数据库。
    Store& store_;
    /// Worker pool outlives all retained futures. 生命周期覆盖所有保留 future 的工作池。
    Resources& resources_;
    /// Maximum retained candidate comparisons. 最多保留的候选比较数。
    std::size_t capacity_;
    /// First representative of the current size/digest bucket. 当前大小/摘要桶的首个代表。
    std::optional<FileRecord> first_;
    /// FIFO preserves deterministic collision handling. 先进先出确保碰撞处理顺序确定。
    std::deque<Pending> pending_;
};
/// Rebuild byte-verified classes from the store's ordered candidate stream.
/// 从存储层有序候选流重建逐字节验证的等价类。
void partition(const fs::path& root, Store& store, Resources& resources, std::size_t capacity) {
    store.reset_matches();
    Partition partitioner(root, store, resources, capacity);
    store.visit_candidates([&](const FileRecord& record) { partitioner.accept(record); });
    partitioner.flush();
}
/// Recheck all displayed paths before any output; this is not an atomic snapshot.
/// 输出前复核所有展示路径；这不是原子快照。
void validate_results(const fs::path& root, Store& store, bool unique_files) {
    const auto validate = [&](std::string_view path) {
        const auto record = store.cached(path);
        if (!record || stamp_path(native_path(root, record->path)) != record->stamp)
            throw std::runtime_error("file changed before output: " + std::string(path));
    };
    store.visit_matches([&](std::string_view, std::string_view member) { validate(member); });
    if (unique_files)
        store.visit_unique(validate);
}
/// Stream an escaped, bounded-memory report; labels work without color.
/// 流式输出转义报告，内存有界；无色时标签仍表达语义。
void render_results(Store& store, Counters& counters, std::ostream& output, OutputOptions options) {
    const auto green = options.color ? "\033[32m" : "";
    const auto yellow = options.color ? "\033[33m" : "";
    const auto reset = options.color ? "\033[0m" : "";
    std::string previous;
    store.visit_matches([&](std::string_view representative, std::string_view member) {
        if (representative != previous) {
            previous = representative;
            ++counters.groups;
            if (options.pretty)
                output << (counters.groups > 1 ? "\n" : "") << green << "[SAME] Group "
                       << counters.groups << reset << '\n';
        }
        ++counters.matches;
        if (options.pretty)
            output << "  " << green;
        else
            output << green << counters.groups << '\t';
        quoted_path(output, member);
        output << reset << '\n';
    });
    if (options.unique_files) {
        if (options.pretty)
            output << yellow << "\n[UNIQUE] No duplicate in this scan" << reset << '\n';
        store.visit_unique([&](std::string_view path) {
            output << yellow << (options.pretty ? "  " : "0\t");
            quoted_path(output, path);
            output << reset << '\n';
        });
    }
    output.flush();
    if (!output)
        throw std::runtime_error("cannot write results");
}
/// 稳定的自动分派诊断名称。 / Stable automatic-dispatch diagnostic names.
std::string_view dispatch_name(detail::DispatchEvidence::Decision decision) {
    using Decision = detail::DispatchEvidence::Decision;
    switch (decision) {
    case Decision::untested:
        return "explicit";
    case Decision::deferred:
        return "cpu-unprobed";
    case Decision::cpu:
        return "cpu";
    case Decision::gpu:
        return "cuda";
    case Decision::failed:
        return "cpu-probe-failed";
    case Decision::adaptive:
        return "adaptive";
    }
    return "invalid";
}
/// 校准终止原因独立于设备与性能偏好。 / Calibration stop is independent of device/preference.
std::string_view calibration_stop(const detail::DispatchEvidence& evidence) {
    using Stop = detail::DispatchEvidence::StopReason;
    if (evidence.stop_reason == Stop::deadline)
        return "deadline";
    if (evidence.stop_reason == Stop::device_error)
        return "device-error";
    if (evidence.calibration_complete)
        return "complete";
    return evidence.device_validated ? "not-run-device-checked" : "not-completed";
}
/// 返回微秒直方图的分位桶范围，不伪装成精确延迟；空分布为未知。
/// Report a quantile bucket's microsecond range, not exact latency; empty means unknown.
std::string histogram_quantile(const std::array<std::uint64_t, 32>& histogram, unsigned percent) {
    const auto count = std::accumulate(histogram.begin(), histogram.end(), std::uint64_t{});
    if (!count)
        return "unknown";
    const auto rank = count / 100 * percent + (count % 100 * percent + 99) / 100;
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < histogram.size(); ++i) {
        cumulative += histogram[i];
        if (cumulative < rank)
            continue;
        const auto lower = i ? std::uint64_t{1} << i : 0;
        return std::to_string(lower) +
               (i == 31 ? "+" : ".." + std::to_string((std::uint64_t{1} << (i + 1)) - 1));
    }
    return "unknown";
}
/// 固定空间的单次运行观测；无堆栈采样器或跨运行训练的暗示。
/// Fixed-space per-run observations; no implication of stack sampling or cross-run training.
void render_online_profile(const Resources& resources, std::ostream& out, bool pretty) {
    const auto profile = resources.profile_snapshot();
    const auto enabled = resources.profiling_enabled();
    const auto p50 = histogram_quantile(profile.latency_histogram, 50);
    const auto p95 = histogram_quantile(profile.latency_histogram, 95);
    const auto residual95 = histogram_quantile(profile.residual_histogram, 95);
    const auto error =
        profile.predicted_samples ? std::to_string(profile.mean_absolute_error_ms) : "unknown";
    if (pretty) {
        out << "  Online PGO      " << (enabled ? "enabled" : "disabled") << " | "
            << profile.samples << " samples (" << profile.cpu_samples << " CPU | "
            << profile.gpu_samples << " GPU) | " << resources.exploration_jobs()
            << " exploration jobs\n"
            << "  Model coverage  " << profile.cpu_known_bands << " CPU | "
            << profile.gpu_known_bands << " GPU observed size bands\n"
            << "  Model error     " << error << " ms absolute-error EWMA | "
            << profile.predicted_samples << " predictions checked | p95 residual " << residual95
            << " us\n"
            << "  Sample latency  p50 " << p50 << " us | p95 " << p95
            << " us (bucket ranges; service incl. I/O)\n";
        return;
    }
    out << "pgo_enabled=" << enabled << " pgo_samples=" << profile.samples
        << " pgo_exploration_jobs=" << resources.exploration_jobs()
        << " pgo_cpu_samples=" << profile.cpu_samples << " pgo_gpu_samples=" << profile.gpu_samples
        << " pgo_cpu_known_bands=" << profile.cpu_known_bands
        << " pgo_gpu_known_bands=" << profile.gpu_known_bands
        << " pgo_rejected_samples=" << profile.rejected_samples
        << " pgo_predicted_samples=" << profile.predicted_samples << " pgo_mae_ms=" << error
        << " pgo_residual_p95_bucket_us=" << residual95 << " pgo_latency_p50_bucket_us=" << p50
        << " pgo_latency_p95_bucket_us=" << p95 << '\n';
}
/// Human-facing profile; machine metrics use a separate unchanged renderer.
/// 面向人的统计展示；机器统计使用独立且保持不变的渲染器。
void render_pretty_profile(const Counters& counters, const Resources& resources,
                           const std::array<double, 5>& phases, double elapsed,
                           std::uint64_t hashed, std::uint64_t compared, std::ostream& out,
                           bool color, bool unique_files) {
    const auto heading = color ? "\033[1;36m" : "";
    const auto value = color ? "\033[36m" : "";
    const auto reset = color ? "\033[0m" : "";
    const auto row = [&](std::string_view label, const std::string& text) {
        out << "  " << std::left << std::setw(16) << label << value << text << reset << '\n';
    };
    out << '\n'
        << heading << "Summary" << reset << "\n"
        << "-----------------------------------------\n";
    // Consolidate result counts and state metadata into the opt-in diagnostic table.
    // 将结果计数与状态元数据统一放入显式启用的诊断表。
    row("Groups", std::to_string(counters.groups));
    row("Matching files", std::to_string(counters.matches));
    row("Unique files", std::to_string(counters.scanned - counters.matches) +
                            (!unique_files && counters.scanned != counters.matches
                                 ? " (hidden; --unique-files to show)"
                                 : ""));
    row("Database", ".same/state.db | " +
                        human_bytes(static_cast<double>(counters.database_bytes)) + " | " +
                        std::to_string(counters.scanned) + " records | committed");
    row("Files", std::to_string(counters.scanned) + " scanned | " +
                     std::to_string(counters.hashed) + " hashed | " +
                     std::to_string(counters.cached) + " cached");
    row("Data", human_bytes(static_cast<double>(counters.scanned_bytes)) + " scanned | " +
                    human_bytes(static_cast<double>(counters.cached_bytes)) + " cached");
    row("Read", human_bytes(static_cast<double>(hashed) + static_cast<double>(compared)) +
                    " total | " + human_bytes(static_cast<double>(hashed)) + " hash | " +
                    human_bytes(static_cast<double>(compared)) + " compare");
    row("Initialize", human_duration(phases[0]));
    row("Scan", human_duration(std::max(0.0, phases[1] - counters.hash_wait_ms)) +
                    " (coordinator incl. walk wait)");
    row("Hash wait", human_duration(counters.hash_wait_ms) + " (submit + join)");
    row("Hash work", human_duration(counters.hash_work_ms) + " (summed workers; overlaps scan)");
    row("Walk wait", human_duration(counters.walk_wait_ms) + " (coordinator wait)");
    row("Enumerate work", human_duration(counters.enumerate_work_ms) + " (summed workers)");
    row("Metadata work", human_duration(counters.metadata_work_ms) + " (summed workers)");
    row("Database work", human_duration(counters.database_work_ms) + " (coordinator)");
    row("Pipeline", human_duration(phases[1]) + " (Scan + Hash wait; excludes Hash work sum)");
    row("Compare", human_duration(phases[2]));
    row("Validate", human_duration(phases[3]));
    row("Output", human_duration(phases[4]));
    row("Elapsed", human_duration(elapsed));
    const auto rate = elapsed > 0 ? (static_cast<double>(hashed) + static_cast<double>(compared)) /
                                        (elapsed / 1000)
                                  : 0;
    row("Read rate", human_bytes(rate) + "/s");
    row("Backend", std::to_string(resources.gpu_workers()) + " GPU workers | " +
                       std::to_string(resources.fallbacks()) + " CPU fallbacks");
    row("CPU size route",
        std::to_string(resources.cpu_routed_hashes()) + " hash attempts (policy, not failure)");
    const auto& dispatch = resources.dispatch_evidence();
    const auto [cpu_attempts, gpu_attempts] = resources.hash_attempts();
    row("Hash backends",
        std::to_string(cpu_attempts) + " CPU | " + std::to_string(gpu_attempts) + " GPU attempts");
    const auto [overflow, spill] = resources.route_counts();
    row("Busy routing",
        std::to_string(overflow) + " GPU overflow | " + std::to_string(spill) + " CPU spill jobs");
    row("GPU input", human_bytes(static_cast<double>(resources.gpu_block_bytes())));
    row("Auto dispatch", std::string(dispatch_name(dispatch.decision)) + " | " +
                             human_duration(dispatch.setup_ms) + " setup (included in scan)");
    row("Calibration", std::string(calibration_stop(dispatch)) + " | device " +
                           (dispatch.device_validated ? "validated" : "not-validated") +
                           " | auto service " +
                           (resources.gpu_service_enabled() ? "active" : "inactive"));
    if (dispatch.elapsed_ms > 0) {
        row("Probe block", dispatch.block_complete
                               ? human_duration(dispatch.cpu_block_ms) + " CPU | " +
                                     human_duration(dispatch.gpu_block_ms) + " GPU"
                               : "unknown (incomplete)");
        row("Probe stream", dispatch.stream_complete
                                ? human_duration(dispatch.cpu_stream_ms) + " CPU | " +
                                      human_duration(dispatch.gpu_stream_ms) + " GPU"
                                : "unknown (incomplete)");
    }
    render_online_profile(resources, out, true);
}
/// Print phase wall times and successful read bytes, not CPU time or physical I/O.
/// 输出各阶段墙钟耗时及成功读取字节，不代表 CPU 时间或物理 I/O。
void render_profile(const Counters& counters, const Resources& resources,
                    const std::array<double, 5>& phases, std::ostream& diagnostics,
                    OutputOptions options) {
    const auto [hash_bytes, compare_bytes] = resources.read_bytes();
    const double elapsed = std::accumulate(phases.begin(), phases.end(), 0.0);
    const double rate =
        elapsed > 0 ? (static_cast<double>(hash_bytes) + static_cast<double>(compare_bytes)) /
                          1048576.0 / (elapsed / 1000.0)
                    : 0;
    // Local formatting leaves caller flags and locale untouched.
    // 局部格式化保留调用方格式和区域设置。
    std::ostringstream profile;
    profile.imbue(std::locale::classic());
    profile << std::fixed << std::setprecision(3);
    if (options.diagnostics_pretty) {
        render_pretty_profile(counters, resources, phases, elapsed, hash_bytes, compare_bytes,
                              profile, options.diagnostics_color, options.unique_files);
        diagnostics << profile.str();
        return;
    }
    profile << "scanned=" << counters.scanned << " hashed=" << counters.hashed
            << " cached=" << counters.cached << " groups=" << counters.groups
            << " matches=" << counters.matches << " gpu_workers=" << resources.gpu_workers()
            << " cpu_fallbacks=" << resources.fallbacks() << '\n';
    profile << "database_bytes=" << counters.database_bytes
            << " database_records=" << counters.scanned << '\n';
    profile << "unique=" << counters.scanned - counters.matches
            << " scanned_bytes=" << counters.scanned_bytes
            << " cached_bytes=" << counters.cached_bytes;
    profile << " " << "hash_read_bytes=" << hash_bytes << " compare_read_bytes=" << compare_bytes
            << " read_bytes=" << hash_bytes + compare_bytes;
    profile << " " << "init_ms=" << phases[0] << " scan_ms=" << phases[1]
            << " compare_ms=" << phases[2] << " validate_ms=" << phases[3]
            << " output_ms=" << phases[4];
    profile << " " << "elapsed_ms=" << elapsed << " read_mib_s=" << rate << '\n';
    profile << "scan_work_ms=" << std::max(0.0, phases[1] - counters.hash_wait_ms)
            << " hash_wait_ms=" << counters.hash_wait_ms
            << " hash_work_ms=" << counters.hash_work_ms << '\n';
    profile << "walk_wait_ms=" << counters.walk_wait_ms
            << " enumerate_work_ms=" << counters.enumerate_work_ms
            << " metadata_work_ms=" << counters.metadata_work_ms
            << " database_work_ms=" << counters.database_work_ms
            << " walk_task_peak=" << counters.walk_task_peak
            << " walk_result_peak=" << counters.walk_result_peak << '\n';
    profile << "cpu_routed_hashes=" << resources.cpu_routed_hashes() << '\n';
    const auto& dispatch = resources.dispatch_evidence();
    const auto [cpu_attempts, gpu_attempts] = resources.hash_attempts();
    const auto [overflow, spill] = resources.route_counts();
    profile << "cpu_hashes=" << cpu_attempts << " gpu_hashes=" << gpu_attempts
            << " gpu_overflow_jobs=" << overflow << " cpu_spill_jobs=" << spill
            << " gpu_block_bytes=" << resources.gpu_block_bytes()
            << " gpu_block_preferred=" << dispatch.block_gpu_preferred
            << " gpu_stream_preferred=" << dispatch.stream_gpu_preferred
            << " auto_backend=" << dispatch_name(dispatch.decision)
            << " gpu_setup_ms=" << dispatch.setup_ms
            << " probe_mixed_cpu_ms=" << dispatch.mixed_cpu_ms
            << " probe_mixed_gpu_ms=" << dispatch.mixed_gpu_ms
            << " expected_gpu_saving_ms=" << dispatch.expected_saving_ms
            << " calibration_ms=" << dispatch.elapsed_ms
            << " probe_cpu_block_ms=" << dispatch.cpu_block_ms
            << " probe_gpu_block_ms=" << dispatch.gpu_block_ms
            << " probe_cpu_stream_ms=" << dispatch.cpu_stream_ms
            << " probe_gpu_stream_ms=" << dispatch.gpu_stream_ms << '\n';
    profile << "calibration_stop=" << calibration_stop(dispatch)
            << " probe_block_complete=" << dispatch.block_complete
            << " probe_stream_complete=" << dispatch.stream_complete
            << " gpu_device_validated=" << dispatch.device_validated
            << " gpu_service_enabled=" << resources.gpu_service_enabled() << '\n';
    render_online_profile(resources, profile, false);
    diagnostics << profile.str();
}
} // namespace
int run(const fs::path& root, const Config& config, std::ostream& output,
        std::ostream& diagnostics) {
    return run(root, config, output, diagnostics, {});
}
int run(const fs::path& root, const Config& config, std::ostream& output, std::ostream& diagnostics,
        OutputOptions options) {
    const auto start = Clock::now();
    WorkspaceLock workspace_lock(root);
    prepare_state(root);
    RunLock lock(root / ".same" / "run.lock");
    Store store(root / ".same" / "state.db");
    Resources resources(config);
    Counters counters;
    const auto initialized = Clock::now();
    scan(root, config, store, resources, counters, options.recursive);
    const auto scanned = Clock::now();
    partition(root, store, resources, config.queue_capacity);
    resources.wait_idle();
    const auto compared = Clock::now();
    validate_results(root, store, options.unique_files);
    // end_scan committed exactly the visited records; measure before emitting any output.
    // end_scan 已提交全部已访问记录；在任何输出前读取文件长度。
    counters.database_bytes = fs::file_size(root / ".same" / "state.db");
    const auto validated = Clock::now();
    render_results(store, counters, output, options);
    const auto finished = Clock::now();
    if (options.summary)
        render_profile(counters, resources,
                       {milliseconds(start, initialized), milliseconds(initialized, scanned),
                        milliseconds(scanned, compared), milliseconds(compared, validated),
                        milliseconds(validated, finished)},
                       diagnostics, options);
    if (config.backend == "cuda" && resources.gpu_workers() < config.workers)
        diagnostics << "CUDA initialization or input registration failed for some workers; using "
                       "CPU fallback.\n";
    if (resources.dispatch_evidence().decision == detail::DispatchEvidence::Decision::failed)
        diagnostics << "CUDA probe failed; auto selected CPU.\n";
    return 0;
}
} // namespace same
