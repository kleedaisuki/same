#include "same/application.hpp"
#include "same/detail/completion_jobs.hpp"
#include "same/files.hpp"
#include "same/resources.hpp"
#include "same/run_lock.hpp"
#include "same/store.hpp"
#include "same/telemetry.hpp"
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
    // 资源工作线程已选择独有后端及匹配缓冲，应用层不二次路由。
    // The worker has selected its private backend and matching buffer; do not reroute here.
    auto& selected = worker.compute;
    const bool gpu = selected->name() == "cuda";
    if (gpu)
        ++worker.gpu_hashes;
    else
        ++worker.cpu_hashes;
    auto hasher = selected->hasher();
    std::uint64_t total = 0;
    for (;;) {
        const auto count = reader.read(worker.first);
        worker.hash_bytes += count;
        if (gpu)
            worker.gpu_hash_bytes += count;
        else
            worker.cpu_hash_bytes += count;
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
/// Stream the tree into bounded worker jobs; keep every SQLite mutation on this thread.
/// 流式遍历目录并提交有界任务；所有 SQLite 修改留在当前线程。
void scan(const fs::path& root, const Config& config, Store& store, Resources& resources,
          Counters& counters, bool recursive, telemetry::Telemetry* trace = nullptr,
          Clock::time_point trace_start = {}) {
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
    // 单调提交编号保证所有工作线程的跨度 ID 唯一。
    // Monotonic submission IDs keep trace spans unique across every worker.
    std::uint64_t next_hash_span = 16;
    auto submit = [&](HashInput input) {
        const auto submitted = Clock::now();
        const auto bytes = input.record.stamp.size;
        const auto span_id = next_hash_span++;
        auto operation = [root, trace, trace_start, span_id, gpu_min_bytes = config.gpu_min_bytes,
                          record = std::move(input.record),
                          opened = std::move(input.reader)](Worker& worker) mutable {
            // 每64个小任务抽样；合格任务全采样，关闭时不修改采样状态。
            // Sample every 64th small task and every eligible task; disabled leaves no sample
            // state.
            const bool sample = worker.profile_enabled &&
                                (record.stamp.size >= gpu_min_bytes ||
                                 (++worker.profile_sequence &
                                  (detail::RoutingParameters::small_sample_period - 1)) == 0);
            const auto cpu_before = worker.cpu_hashes;
            const auto gpu_before = worker.gpu_hashes;
            const auto start = Clock::now();
            // 固定事件只复用已有采样和时钟；不进入调度器锁，不读取文件内容。
            // Fixed events reuse sampling and clocks; no scheduler lock or file contents.
            auto report = [&](Clock::time_point end, bool success) noexcept {
                if (!trace)
                    return;
                const auto gpu_attempts = worker.gpu_hashes - gpu_before;
                const auto cpu_attempts = worker.cpu_hashes - cpu_before;
                const bool retry = gpu_attempts + cpu_attempts > 1;
                if (!sample && !retry && success)
                    return;
                telemetry::Event event;
                event.type = sample ? "span" : "log";
                event.name = sample ? "hash" : (success ? "hash.fallback" : "hash.error");
                event.severity = success ? (retry ? "warn" : "info") : "error";
                event.backend = retry ? "gpu+cpu" : (gpu_attempts ? "gpu" : "cpu");
                event.message = record.path;
                event.span_id = span_id;
                event.parent_span_id = 3;
                event.worker = static_cast<std::int64_t>(worker.index);
                event.bytes = record.stamp.size;
                event.time_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(start - trace_start)
                        .count());
                event.duration_ns = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
                event.value = success ? 1 : 0;
                trace->emit(event);
                if (sample && retry) {
                    event.type = "log";
                    event.name = "hash.fallback";
                    trace->emit(event);
                }
            };
            FileRecord hashed;
            Clock::time_point ended;
            try {
                hashed = worker.execute([&] {
                    return hash_file(root, record, worker, gpu_min_bytes, std::move(opened));
                });
                ended = Clock::now();
            } catch (...) {
                report(Clock::now(), false);
                throw;
            }
            const auto elapsed = milliseconds(start, ended);
            report(ended, true);
            // 复用已有计时；仅完整成功且没有重试的实际后端样本进入模型。
            // Reuse existing timing; only successful single-attempt samples train the model.
            if (sample && worker.cpu_hashes - cpu_before + worker.gpu_hashes - gpu_before == 1)
                worker.sample = {hashed.stamp.size, elapsed, worker.gpu_hashes != gpu_before, true};
            return HashResult{std::move(hashed), elapsed};
        };
        // 统一队列只携带载荷；工作线程在锁外选择并学习自己的后端。
        // The common queue carries bytes only; each worker selects and learns outside its lock.
        pending.submit_hash(std::move(operation), bytes);
        counters.hash_wait_ms += milliseconds(submitted, Clock::now());
        if (pending.pending() >= config.queue_capacity)
            drain();
    };
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
        submit({std::move(record), std::move(entry->reader)});
    }
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
    if (pretty) {
        out << "  Online PGO      " << (enabled ? "enabled" : "disabled") << " | "
            << profile.samples << " samples (" << profile.cpu_samples << " CPU | "
            << profile.gpu_samples << " GPU) | " << resources.exploration_jobs()
            << " exploration jobs\n"
            << "  Model coverage  " << profile.cpu_known_bands << " CPU | "
            << profile.gpu_known_bands << " GPU observed worker-band pairs\n"
            << "  Model residual  " << profile.predicted_samples
            << " predictions checked | p95 residual " << residual95 << " us\n"
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
        << " pgo_predicted_samples=" << profile.predicted_samples
        << " pgo_residual_p95_bucket_us=" << residual95 << " pgo_latency_p50_bucket_us=" << p50
        << " pgo_latency_p95_bucket_us=" << p95 << '\n';
}
/// 空闲后展示每工作线程的真实贡献；模型误差不跨线程合并。
/// Show worker contributions after idle; never merge independent model error EWMAs.
void render_worker_profiles(const Resources& resources, std::ostream& out, bool pretty) {
    const auto workers = resources.worker_profiles();
    out << (pretty ? "  Worker count    " : "worker_count=") << workers.size() << '\n';
    std::uint64_t cpu_bytes = 0, gpu_bytes = 0;
    std::size_t gpu_peak = 0, gpu_available = 0;
    for (const auto& worker : workers) {
        cpu_bytes += worker.cpu_hash_bytes;
        gpu_bytes += worker.gpu_hash_bytes;
        gpu_peak = std::max(gpu_peak, worker.gpu_peak_concurrency);
        gpu_available += worker.gpu_enabled ? 1 : 0;
    }
    if (pretty)
        out << "  Backend reads   " << human_bytes(static_cast<double>(cpu_bytes)) << " CPU | "
            << human_bytes(static_cast<double>(gpu_bytes)) << " GPU | peak " << gpu_peak
            << " concurrent GPU tasks\n";
    else
        out << "cpu_hash_read_bytes=" << cpu_bytes << " gpu_hash_read_bytes=" << gpu_bytes
            << " gpu_peak_concurrency=" << gpu_peak << " gpu_available_workers=" << gpu_available
            << '\n';
    for (const auto& worker : workers) {
        const auto& profile = worker.snapshot;
        const auto error =
            profile.predicted_samples ? std::to_string(profile.mean_absolute_error_ms) : "unknown";
        if (pretty) {
            out << "  Worker " << worker.index << "        " << worker.cpu_hashes << " CPU | "
                << worker.gpu_hashes << " GPU attempts | " << worker.cpu_hash_bytes << " CPU B | "
                << worker.gpu_hash_bytes << " GPU B\n"
                << "                  " << profile.samples << " samples | local error EWMA "
                << error << " ms | GPU setup " << human_duration(worker.setup_ms) << " | init "
                << (worker.gpu_attempted ? "attempted" : "not-needed") << " | GPU "
                << (worker.gpu_enabled ? "available" : "unavailable") << " | "
                << worker.gpu_init_failures << " init failures | " << worker.fallbacks
                << " retries | " << worker.cold_start_cpu << " cold-start CPU bypasses\n";
            continue;
        }
        const auto prefix = "worker." + std::to_string(worker.index) + ".";
#define WORKER_VALUE(field) out << prefix << #field << '=' << worker.field << ' '
        WORKER_VALUE(cpu_hashes);
        WORKER_VALUE(gpu_hashes);
        WORKER_VALUE(cpu_hash_bytes);
        WORKER_VALUE(gpu_hash_bytes);
        WORKER_VALUE(hash_bytes);
        WORKER_VALUE(compare_bytes);
        WORKER_VALUE(cpu_routed_hashes);
        WORKER_VALUE(setup_ms);
        WORKER_VALUE(gpu_attempted);
        WORKER_VALUE(gpu_enabled);
        WORKER_VALUE(gpu_init_failures);
        WORKER_VALUE(fallbacks);
        WORKER_VALUE(exploration_jobs);
        WORKER_VALUE(model_cpu);
        WORKER_VALUE(model_gpu);
        WORKER_VALUE(static_cpu);
        WORKER_VALUE(static_gpu);
        WORKER_VALUE(size_cpu);
        WORKER_VALUE(unavailable_cpu);
        WORKER_VALUE(cold_start_cpu);
        WORKER_VALUE(gpu_inflight_at_selection);
        WORKER_VALUE(gpu_peak_concurrency);
        WORKER_VALUE(contended_samples);
        WORKER_VALUE(cpu_block_bytes);
        WORKER_VALUE(gpu_block_bytes);
        WORKER_VALUE(device_budget_bytes);
#undef WORKER_VALUE
        out << prefix << "samples=" << profile.samples << ' ' << prefix
            << "cpu_samples=" << profile.cpu_samples << ' ' << prefix
            << "gpu_samples=" << profile.gpu_samples << ' ' << prefix
            << "predicted_samples=" << profile.predicted_samples << ' ' << prefix
            << "cpu_known_bands=" << profile.cpu_known_bands << ' ' << prefix
            << "gpu_known_bands=" << profile.gpu_known_bands << ' ' << prefix
            << "local_error_ewma_ms=" << error << '\n';
    }
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
    row("Backend", std::to_string(resources.gpu_workers()) + " workers initialized CUDA | " +
                       std::to_string(resources.fallbacks()) + " CPU fallbacks");
    row("CPU size route",
        std::to_string(resources.cpu_routed_hashes()) + " hash attempts (policy, not failure)");
    const auto [cpu_attempts, gpu_attempts] = resources.hash_attempts();
    row("Hash backends",
        std::to_string(cpu_attempts) + " CPU | " + std::to_string(gpu_attempts) + " GPU attempts");
    row("Scheduling", "one queue | worker-local CPU/GPU selection and models");
    row("GPU input", human_bytes(static_cast<double>(resources.gpu_block_bytes())));
    render_online_profile(resources, out, true);
    render_worker_profiles(resources, out, true);
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
    const auto [cpu_attempts, gpu_attempts] = resources.hash_attempts();
    profile << "cpu_hashes=" << cpu_attempts << " gpu_hashes=" << gpu_attempts
            << " gpu_block_bytes=" << resources.gpu_block_bytes()
            << " scheduler=worker-local single_queue=1" << '\n';
    render_online_profile(resources, profile, false);
    render_worker_profiles(resources, profile, false);
    diagnostics << profile.str();
}

/// Serialize effective settings once, never on a worker or when telemetry is disabled.
/// 有效配置仅序列化一次，工作线程和禁用遥测路径均不执行。
std::string telemetry_config(const Config& config, OutputOptions options) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{";
#define CONFIG_NUMBER(field) out << "\"" #field "\":" << config.field << ','
    CONFIG_NUMBER(workers);
    CONFIG_NUMBER(metadata_workers);
    CONFIG_NUMBER(gpu_min_bytes);
    CONFIG_NUMBER(block_bytes);
    CONFIG_NUMBER(memory_bytes);
    CONFIG_NUMBER(device_memory_bytes);
    CONFIG_NUMBER(queue_capacity);
    CONFIG_NUMBER(rehash);
    CONFIG_NUMBER(pgo);
    CONFIG_NUMBER(telemetry);
    CONFIG_NUMBER(telemetry_queue_capacity);
    CONFIG_NUMBER(telemetry_retention_runs);
    CONFIG_NUMBER(telemetry_max_events);
#undef CONFIG_NUMBER
    out << "\"recursive\":" << options.recursive << ",\"unique_files\":" << options.unique_files
        << ",\"summary\":" << options.summary << ",\"backend\":";
    quoted_path(out, config.backend);
    out << '}';
    return out.str();
}
/// Preserve raw counter/model state, rather than reparsing a human report.
/// 保存原始计数和模型状态，不反向解析展示文本；调用者必须已经等待工作线程空闲。
void capture_telemetry(telemetry::FinalRecord& final, const Counters& counters,
                       const Resources* resources, const std::array<double, 5>& phases) {
    auto metric = [&](std::string name, double value, std::string unit = "count") {
        final.metrics.push_back({std::move(name), value, std::move(unit)});
    };
#define COUNTER(field, unit) metric(#field, static_cast<double>(counters.field), unit)
    COUNTER(scanned, "count");
    COUNTER(database_bytes, "bytes");
    COUNTER(scanned_bytes, "bytes");
    COUNTER(cached_bytes, "bytes");
    COUNTER(hashed, "count");
    COUNTER(hash_wait_ms, "ms");
    COUNTER(hash_work_ms, "ms");
    COUNTER(walk_wait_ms, "ms");
    COUNTER(enumerate_work_ms, "ms");
    COUNTER(metadata_work_ms, "ms");
    COUNTER(database_work_ms, "ms");
    COUNTER(walk_task_peak, "count");
    COUNTER(walk_result_peak, "count");
    COUNTER(cached, "count");
    COUNTER(groups, "count");
    COUNTER(matches, "count");
#undef COUNTER
    constexpr std::array<const char*, 5> names{"initialize_ms", "pipeline_ms", "compare_ms",
                                               "validate_ms", "output_ms"};
    for (std::size_t i = 0; i < phases.size(); ++i)
        metric(names[i], phases[i], "ms");
    const auto elapsed = std::accumulate(phases.begin(), phases.end(), 0.0);
    metric("elapsed_ms", elapsed, "ms");
    metric("scan_work_ms", std::max(0.0, phases[1] - counters.hash_wait_ms), "ms");
    metric("unique", static_cast<double>(counters.scanned - counters.matches));
    auto parameter = [&](std::string category, std::string name, auto value) {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::setprecision(17) << value;
        final.parameters.push_back({std::move(category), std::move(name), text.str()});
    };
#define ROUTING(field) parameter("routing", #field, detail::RoutingParameters::field)
    ROUTING(cpu_advantage_factor);
    ROUTING(exploration_period);
    ROUTING(initial_gpu_explorations);
    ROUTING(small_sample_period);
    ROUTING(static_gpu_floor_bytes);
    ROUTING(gpu_block_bytes);
#undef ROUTING
    parameter("model", "smoothing_alpha", detail::OnlineModel::smoothing_alpha);
    parameter("model", "band_shift", detail::OnlineModel::band_shift);
    parameter("model", "band_count", detail::OnlineModel::band_count);
    parameter("model", "backend_count", detail::OnlineModel::backend_count);
    parameter("model", "training_scope", "worker-local-current-run-only");
    if (!resources)
        return;
    metric("gpu_workers", static_cast<double>(resources->gpu_workers()));
    metric("cpu_fallbacks", static_cast<double>(resources->fallbacks()));
    metric("cpu_routed_hashes", static_cast<double>(resources->cpu_routed_hashes()));
    metric("gpu_block_bytes", static_cast<double>(resources->gpu_block_bytes()), "bytes");
    const auto workers = resources->worker_profiles();
    metric("worker_count", static_cast<double>(workers.size()));
    metric("gpu_available_workers", static_cast<double>(std::count_if(
                                        workers.begin(), workers.end(),
                                        [](const auto& worker) { return worker.gpu_enabled; })));
    metric("pgo_enabled", resources->profiling_enabled());
    metric("pgo_exploration_jobs", static_cast<double>(resources->exploration_jobs()));
    const auto [hashed, compared] = resources->read_bytes();
    metric("hash_read_bytes", static_cast<double>(hashed), "bytes");
    metric("compare_read_bytes", static_cast<double>(compared), "bytes");
    metric("read_bytes", static_cast<double>(hashed) + static_cast<double>(compared), "bytes");
    metric("read_mib_s",
           elapsed > 0 ? (static_cast<double>(hashed) + static_cast<double>(compared)) / 1048576.0 /
                             (elapsed / 1000.0)
                       : 0,
           "MiB/s");
    const auto [cpu, gpu] = resources->hash_attempts();
    metric("cpu_hashes", static_cast<double>(cpu));
    metric("gpu_hashes", static_cast<double>(gpu));
    parameter("routing", "model_scope", "worker-local-current-run");
    const auto profile = resources->profile_snapshot();
#define PROFILE(field, unit) metric("pgo." #field, static_cast<double>(profile.field), unit)
    PROFILE(samples, "count");
    PROFILE(cpu_samples, "count");
    PROFILE(gpu_samples, "count");
    PROFILE(rejected_samples, "count");
    PROFILE(predicted_samples, "count");
    PROFILE(cpu_known_bands, "count");
    PROFILE(gpu_known_bands, "count");
#undef PROFILE
    for (std::size_t i = 0; i < profile.latency_histogram.size(); ++i) {
        metric("pgo.latency_bucket_us." + std::to_string(i),
               static_cast<double>(profile.latency_histogram[i]));
        metric("pgo.residual_bucket_us." + std::to_string(i),
               static_cast<double>(profile.residual_histogram[i]));
    }
    for (const auto& worker : workers) {
        const auto scope = "worker." + std::to_string(worker.index);
#define WORKER_METRIC(field, unit)                                                                 \
    metric(scope + "." #field, static_cast<double>(worker.field), unit)
        WORKER_METRIC(cpu_hashes, "count");
        WORKER_METRIC(gpu_hashes, "count");
        WORKER_METRIC(cpu_hash_bytes, "bytes");
        WORKER_METRIC(gpu_hash_bytes, "bytes");
        WORKER_METRIC(hash_bytes, "bytes");
        WORKER_METRIC(compare_bytes, "bytes");
        WORKER_METRIC(cpu_routed_hashes, "count");
        WORKER_METRIC(setup_ms, "ms");
        WORKER_METRIC(gpu_attempted, "bool");
        WORKER_METRIC(gpu_enabled, "bool");
        WORKER_METRIC(gpu_init_failures, "count");
        WORKER_METRIC(fallbacks, "count");
        WORKER_METRIC(exploration_jobs, "count");
        WORKER_METRIC(model_cpu, "count");
        WORKER_METRIC(model_gpu, "count");
        WORKER_METRIC(static_cpu, "count");
        WORKER_METRIC(static_gpu, "count");
        WORKER_METRIC(size_cpu, "count");
        WORKER_METRIC(unavailable_cpu, "count");
        WORKER_METRIC(cold_start_cpu, "count");
        WORKER_METRIC(gpu_inflight_at_selection, "count");
        WORKER_METRIC(gpu_peak_concurrency, "count");
        WORKER_METRIC(contended_samples, "count");
#undef WORKER_METRIC
        parameter(scope, "cpu_block_bytes", worker.cpu_block_bytes);
        parameter(scope, "gpu_block_bytes", worker.gpu_block_bytes);
        parameter(scope, "device_budget_bytes", worker.device_budget_bytes);
        const auto& local = worker.snapshot;
#define LOCAL_PROFILE(field, unit)                                                                 \
    metric(scope + ".pgo." #field, static_cast<double>(local.field), unit)
        LOCAL_PROFILE(samples, "count");
        LOCAL_PROFILE(cpu_samples, "count");
        LOCAL_PROFILE(gpu_samples, "count");
        LOCAL_PROFILE(rejected_samples, "count");
        LOCAL_PROFILE(predicted_samples, "count");
        LOCAL_PROFILE(cpu_known_bands, "count");
        LOCAL_PROFILE(gpu_known_bands, "count");
        LOCAL_PROFILE(mean_absolute_error_ms, "ms");
#undef LOCAL_PROFILE
        for (std::size_t i = 0; i < local.latency_histogram.size(); ++i) {
            metric(scope + ".pgo.latency_bucket_us." + std::to_string(i),
                   static_cast<double>(local.latency_histogram[i]));
            metric(scope + ".pgo.residual_bucket_us." + std::to_string(i),
                   static_cast<double>(local.residual_histogram[i]));
        }
        for (const auto& band : worker.parameters) {
            const auto category =
                scope + ".model." + (band.gpu ? "gpu." : "cpu.") + std::to_string(band.band_index);
            parameter(category, "known", band.known);
            parameter(category, "samples", band.samples);
            parameter(category, "cost_ms_per_byte", band.cost_ms_per_byte);
            parameter(category, "error_ms_per_byte", band.error_ms_per_byte);
        }
    }
}
/// Append final writer health; legacy elapsed excludes this explicit exit drain.
/// 附加写入器终态；旧 elapsed 不含明确展示的退出排空耗时。
void render_telemetry(const telemetry::Telemetry* trace, const telemetry::Stats& stats,
                      double total_ms, std::ostream& out, bool pretty) {
    if (!trace) {
        out << (pretty ? "  Telemetry       disabled\n" : "telemetry_enabled=0\n");
        return;
    }
    if (pretty) {
        out << "  Telemetry       .same/telemetry.db | " << stats.status.c_str() << '\n'
            << "  Run ID          " << trace->run_id() << '\n'
            << "  Trace records   " << stats.accepted << " accepted | " << stats.persisted
            << " persisted | " << stats.dropped << " dropped | " << stats.errors << " errors\n"
            << "  Trace pressure  " << stats.queue_high_water << " queued peak | "
            << stats.truncated << " truncated\n"
            << "  Telemetry drain " << human_duration(stats.drain_ms) << " (exit wait)\n"
            << "  Total incl. I/O " << human_duration(total_ms) << " (includes telemetry drain)\n";
    } else {
        out << "telemetry_enabled=1 telemetry_db=.same/telemetry.db telemetry_run_id="
            << trace->run_id() << " telemetry_status=" << stats.status.c_str()
            << " telemetry_accepted=" << stats.accepted
            << " telemetry_persisted=" << stats.persisted << " telemetry_dropped=" << stats.dropped
            << " telemetry_write_errors=" << stats.errors
            << " telemetry_truncated=" << stats.truncated
            << " telemetry_queue_high_water=" << stats.queue_high_water
            << " telemetry_drain_ms=" << stats.drain_ms
            << " total_including_telemetry_ms=" << total_ms << '\n';
    }
}
} // namespace
int run(const fs::path& root, const Config& config, std::ostream& output,
        std::ostream& diagnostics) {
    return run(root, config, output, diagnostics, {});
}
int run(const fs::path& root, const Config& config, std::ostream& output, std::ostream& diagnostics,
        OutputOptions options) {
    const auto start = Clock::now();
    const auto wall_start = config.telemetry ? std::chrono::system_clock::now()
                                             : std::chrono::system_clock::time_point{};
    WorkspaceLock workspace_lock(root);
    prepare_state(root);
    RunLock lock(root / ".same" / "run.lock");
    // 生命周期日志先于业务存储；所有工作线程先停止，写入器最后在锁内收尾。
    // Journal precedes business storage; workers stop before writer finalization under the lock.
    std::unique_ptr<telemetry::Telemetry> trace;
    if (config.telemetry) {
        try {
            telemetry::RunInfo info;
            info.version = SAME_VERSION;
            info.started_unix_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(wall_start.time_since_epoch())
                    .count());
            info.command = "scan";
            info.root = path_key(root);
            info.config_json = telemetry_config(config, options);
            telemetry::Options settings;
            settings.queue_capacity = config.telemetry_queue_capacity;
            settings.event_cap = config.telemetry_max_events;
            settings.retain_runs = config.telemetry_retention_runs;
            trace = std::make_unique<telemetry::Telemetry>(root / ".same" / "telemetry.db",
                                                           std::move(info), settings);
        } catch (...) {
            diagnostics << "Telemetry warning: cannot allocate run metadata; scan continues.\n";
        }
    }
    telemetry::FinalRecord final;
    Counters counters;
    std::array<double, 5> phases{};
    std::ostringstream summary;
    std::exception_ptr failure;
    std::unique_ptr<Store> store;
    std::unique_ptr<Resources> resources;
    constexpr std::array<const char*, 5> stage_names{"initialize", "scan", "compare", "validate",
                                                     "output"};
    std::size_t stage = 0;
    auto stage_start = start;
    bool incomplete_telemetry = false;
    auto event = [&](std::string_view type, std::string_view name, Clock::time_point begin,
                     Clock::time_point end, std::uint64_t id, std::uint64_t parent,
                     bool success = true) noexcept {
        if (!trace)
            return;
        telemetry::Event record;
        record.type = type;
        record.name = name;
        record.severity = success ? "info" : "error";
        record.span_id = id;
        record.parent_span_id = parent;
        record.time_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(begin - start).count());
        record.duration_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
        record.value = success ? 1 : 0;
        // 阶段事件数量固定且走结束槽，不与抽样文件争用事件预算。
        // Fixed-count phase events use the final slot, never compete with sampled files.
        try {
            final.events.push_back(record);
        } catch (...) {
            incomplete_telemetry = true;
        }
    };
    auto end_stage = [&](Clock::time_point end, bool success = true) {
        phases[stage] = milliseconds(stage_start, end);
        event("span", stage_names[stage], stage_start, end, stage + 2, 1, success);
        stage_start = end;
    };
    event("log", "run.start", start, start, 1, 0);
    try {
        store = std::make_unique<Store>(root / ".same" / "state.db");
        resources = std::make_unique<Resources>(config);
        end_stage(Clock::now());
        stage = 1;
        event("log", "scan.start", stage_start, stage_start, 3, 1);
        scan(root, config, *store, *resources, counters, options.recursive, trace.get(), start);
        end_stage(Clock::now());
        stage = 2;
        event("log", "compare.start", stage_start, stage_start, 4, 1);
        partition(root, *store, *resources, config.queue_capacity);
        resources->wait_idle();
        end_stage(Clock::now());
        stage = 3;
        event("log", "validate.start", stage_start, stage_start, 5, 1);
        validate_results(root, *store, options.unique_files);
        counters.database_bytes = fs::file_size(root / ".same" / "state.db");
        end_stage(Clock::now());
        stage = 4;
        event("log", "output.start", stage_start, stage_start, 6, 1);
        render_results(*store, counters, output, options);
        end_stage(Clock::now());
    } catch (...) {
        failure = std::current_exception();
        if (resources)
            resources->wait_idle();
        end_stage(Clock::now(), false);
        final.status = "failed";
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            try {
                final.error = error.what();
            } catch (...) {
                incomplete_telemetry = true;
            }
        } catch (...) {
            try {
                final.error = "non-standard exception";
            } catch (...) {
                incomplete_telemetry = true;
            }
        }
    }
    if (trace) {
        try {
            capture_telemetry(final, counters, resources.get(), phases);
        } catch (...) {
            incomplete_telemetry = true;
        }
    }
    if (resources && !failure) {
        if (options.summary)
            render_profile(counters, *resources, phases, summary, options);
        std::uint64_t init_failures = 0;
        for (const auto& worker : resources->worker_profiles())
            init_failures += worker.gpu_init_failures;
        if (init_failures)
            diagnostics << "CUDA initialization failed for " << init_failures
                        << " worker contexts; affected workers used CPU fallback.\n";
        if (trace && (resources->fallbacks() || init_failures)) {
            event("log", "backend.fallback", stage_start, stage_start, 1, 0);
            if (!final.events.empty())
                final.events.back().severity = "warn";
        }
    }
    resources.reset();
    store.reset();
    const auto completed = Clock::now();
    event("span", "run", start, completed, 1, 0, !failure);
    event("log", failure ? "run.failed" : "run.completed", completed, completed, 1, 0, !failure);
    if (trace && failure && !final.events.empty())
        final.events.back().message = final.error;
    if (incomplete_telemetry)
        diagnostics << "Telemetry warning: final metadata is incomplete (allocation failure).\n";
    telemetry::Stats stats;
    if (trace)
        stats = trace->finish(std::move(final));
    const auto total_ms = milliseconds(start, Clock::now());
    if (options.summary) {
        diagnostics << summary.str();
        render_telemetry(trace.get(), stats, total_ms, diagnostics, options.diagnostics_pretty);
    }
    // 写入失败不依赖 --summary，避免静默失去整个运行历史。 / Surface writer failure even
    // without --summary so a missing run history is never silently reported as healthy.
    if (stats.errors) {
        diagnostics << "Telemetry warning: ";
        quoted_path(diagnostics, std::string_view(stats.error));
        diagnostics << '\n';
    }
    if (failure)
        std::rethrow_exception(failure);
    return 0;
}
} // namespace same
