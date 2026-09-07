#include "same/application.hpp"
#include "same/files.hpp"
#include "same/resources.hpp"
#include "same/run_lock.hpp"
#include "same/store.hpp"
#include <algorithm>
#include <deque>
#include <future>
#include <optional>
#include <ostream>
#include <stdexcept>

namespace same {
namespace {
namespace fs = std::filesystem;
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
std::size_t read_block(FileReader& reader, std::span<std::byte> buffer) {
    std::size_t count = 0;
    while (count < buffer.size()) {
        const auto read = reader.read(buffer.subspan(count));
        if (!read)
            break;
        count += read;
    }
    return count;
}
/// Hash from a fresh handle; reject size/stamp changes before publishing the digest.
/// 从新句柄计算哈希；发布摘要前拒绝大小或文件戳变化。
FileRecord hash_file(const fs::path& root, FileRecord record, Worker& worker) {
    const auto path = native_path(root, record.path);
    FileReader reader(path);
    unchanged(path, reader, record.stamp);
    auto hasher = worker.compute->hasher();
    std::uint64_t total = 0;
    for (;;) {
        const auto count = reader.read(worker.first);
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
        if (read_block(first, x) != count || read_block(second, y) != count)
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
    /// Files submitted for hashing. 已提交哈希计算的文件数。
    std::size_t hashed = 0;
    /// Records reused after matching stamps. 文件戳匹配后复用的记录数。
    std::size_t cached = 0;
    /// Emitted duplicate equivalence classes. 输出的重复文件等价类数量。
    std::size_t groups = 0;
    /// Emitted members, including representatives. 输出成员数，包含代表文件。
    std::size_t matches = 0;
};
/// Stream the tree into bounded worker jobs; keep every SQLite mutation on this thread.
/// 流式遍历目录并提交有界任务；所有 SQLite 修改留在当前线程。
void scan(const fs::path& root, const Config& config, Store& store, Resources& resources,
          Counters& counters) {
    Ignore ignore(root);
    std::deque<std::future<FileRecord>> pending;
    auto drain = [&] {
        store.save(pending.front().get());
        pending.pop_front();
    };
    store.begin_scan();
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        const auto key = path_key(it->path().lexically_relative(root));
        const auto status = it->symlink_status();
        if (is_reparse_point(it->path()) ||
            (fs::is_directory(status) &&
             (fs::equivalent(it->path(), root / ".same") || ignore.can_prune(key)))) {
            it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(status) || ignore.matches(key, false))
            continue;
        ++counters.scanned;
        FileRecord record{key, stamp_path(it->path()), {}};
        const auto cached = store.cached(key);
        if (!config.rehash && cached && cached->stamp == record.stamp) {
            store.save(*cached);
            ++counters.cached;
            continue;
        }
        ++counters.hashed;
        pending.push_back(resources.submit([root, record = std::move(record)](Worker& worker) {
            return worker.execute([&] { return hash_file(root, record, worker); });
        }));
        // Futures, as well as executable jobs, are bounded: slow early files cannot
        // let completed results accumulate without limit.
        // future 同样有界，避免前面的慢文件导致后续已完成结果无限堆积。
        if (pending.size() >= config.queue_capacity)
            drain();
    }
    while (!pending.empty())
        drain();
    store.end_scan();
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
} // namespace
int run(const fs::path& root, const Config& config, std::ostream& output,
        std::ostream& diagnostics) {
    prepare_state(root);
    RunLock lock(root / ".same" / "run.lock");
    Store store(root / ".same" / "state.db");
    Resources resources(config);
    Counters counters;
    scan(root, config, store, resources, counters);
    partition(root, store, resources, config.queue_capacity);
    // Validate every reported member before emitting anything. This detects ordinary
    // concurrent edits, but a live filesystem is not an atomic snapshot.
    // 输出前复核所有成员；这能检测通常的并发修改，但不构成原子文件系统快照。
    store.visit_matches([&](std::string_view, std::string_view member) {
        const auto record = store.cached(member);
        if (!record || stamp_path(native_path(root, record->path)) != record->stamp)
            throw std::runtime_error("file changed before output: " + std::string(member));
    });
    std::string previous;
    store.visit_matches([&](std::string_view representative, std::string_view member) {
        if (representative != previous) {
            previous = representative;
            ++counters.groups;
        }
        ++counters.matches;
        output << counters.groups << '\t';
        quoted_path(output, member);
        output << '\n';
    });
    output.flush();
    if (!output)
        throw std::runtime_error("cannot write results");
    diagnostics << "scanned=" << counters.scanned << " hashed=" << counters.hashed
                << " cached=" << counters.cached << " groups=" << counters.groups
                << " matches=" << counters.matches << " gpu_workers=" << resources.gpu_workers()
                << " cpu_fallbacks=" << resources.fallbacks() << '\n';
    if (config.backend != "cpu" && resources.gpu_workers() < config.workers)
        diagnostics << "CUDA unavailable or device budget insufficient for some workers; using CPU "
                       "fallback.\n";
    return 0;
}
} // namespace same
