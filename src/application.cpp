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
std::string path_key(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
fs::path native_path(const fs::path& root, const std::string& key) {
    return root / fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(key.data()), key.size()));
}
void unchanged(const fs::path& path, FileReader& reader, const FileStamp& expected) {
    if (reader.stamp() != expected || stamp_path(path) != expected)
        throw std::runtime_error("file changed while processing: " + path_key(path));
}
std::size_t read_block(FileReader& reader, std::span<std::byte> buffer) {
    std::size_t count = 0;
    while (count < buffer.size()) {
        const auto read = reader.read(buffer.subspan(count));
        if (!read) break;
        count += read;
    }
    return count;
}
FileRecord hash_file(const fs::path& root, FileRecord record, Worker& worker) {
    const auto path = native_path(root, record.path);
    FileReader reader(path);
    unchanged(path, reader, record.stamp);
    auto hasher = worker.compute->hasher();
    std::uint64_t total = 0;
    for (;;) {
        const auto count = reader.read(worker.first);
        if (!count) break;
        if (count > record.stamp.size - total)
            throw std::runtime_error("file grew while hashing: " + record.path);
        hasher->update(std::span(worker.first).first(count));
        total += count;
    }
    if (total != record.stamp.size) throw std::runtime_error("file shrank while hashing: " + record.path);
    record.digest = hasher->finish();
    unchanged(path, reader, record.stamp);
    return record;
}
bool equal_files(const fs::path& root, const FileRecord& a, const FileRecord& b, Worker& worker) {
    const auto path_a = native_path(root, a.path), path_b = native_path(root, b.path);
    FileReader first(path_a), second(path_b);
    unchanged(path_a, first, a.stamp);
    unchanged(path_b, second, b.stamp);
    bool equal = a.stamp.size == b.stamp.size;
    auto left = a.stamp.size;
    while (equal && left) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(left, worker.first.size()));
        auto x = std::span(worker.first).first(count), y = std::span(worker.second).first(count);
        if (read_block(first, x) != count || read_block(second, y) != count)
            throw std::runtime_error("file truncated during comparison: " + a.path + " / " + b.path);
        equal = worker.compute->equal(x, y);
        left -= count;
    }
    unchanged(path_a, first, a.stamp);
    unchanged(path_b, second, b.stamp);
    return equal;
}
void quoted_path(std::ostream& out, std::string_view path) {
    constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (unsigned char c : path) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c < 32 || c == 127) out << "\\u00" << digits[c >> 4] << digits[c & 15];
        else out << static_cast<char>(c);
    }
    out << '"';
}
void prepare_state(const fs::path& root) {
    const auto state = root / ".same";
    const auto status = fs::symlink_status(state);
    if (fs::exists(status)) {
        if (!fs::is_directory(status) || is_reparse_point(state))
            throw std::runtime_error(".same must be a real directory, not a link");
    } else fs::create_directory(state);
    for (const auto* name : {"state.db", "state.db-wal", "state.db-shm", "state.db-journal", "run.lock"}) {
        const auto path = state / name;
        const auto entry = fs::symlink_status(path);
        if (fs::exists(entry) && (!fs::is_regular_file(entry) || is_reparse_point(path)))
            throw std::runtime_error("state path must be a regular non-link file: " + path_key(path));
    }
}
struct Counters { std::size_t scanned = 0, hashed = 0, cached = 0, groups = 0, matches = 0; };
void scan(const fs::path& root, const Config& config, Store& store, Resources& resources, Counters& counters) {
    Ignore ignore(root);
    std::deque<std::future<FileRecord>> pending;
    auto drain = [&] { store.save(pending.front().get()); pending.pop_front(); };
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
        if (!fs::is_regular_file(status) || ignore.matches(key, false)) continue;
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
        if (pending.size() >= config.queue_capacity) drain();
    }
    while (!pending.empty()) drain();
    store.end_scan();
}
void partition(const fs::path& root, Store& store, Resources& resources) {
    std::optional<FileRecord> previous;
    store.reset_matches();
    store.visit_candidates([&](const FileRecord& record) {
        if (!previous || previous->stamp.size != record.stamp.size || previous->digest != record.digest)
            store.clear_representatives();
        bool matched = false;
        store.visit_representatives([&](const FileRecord& representative) {
            auto result = resources.submit([root, record, representative](Worker& worker) {
                return worker.execute([&] { return equal_files(root, representative, record, worker); });
            });
            if (!result.get()) return true;
            store.add_match(representative.path, record.path);
            matched = true;
            return false;
        });
        if (!matched) {
            store.add_representative(record);
            store.add_match(record.path, record.path);
        }
        previous = record;
    });
}
}
int run(const fs::path& root, const Config& config, std::ostream& output, std::ostream& diagnostics) {
    prepare_state(root);
    RunLock lock(root / ".same" / "run.lock");
    Store store(root / ".same" / "state.db");
    Resources resources(config);
    Counters counters;
    scan(root, config, store, resources, counters);
    partition(root, store, resources);
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
        if (representative != previous) { previous = representative; ++counters.groups; }
        ++counters.matches;
        output << counters.groups << '\t';
        quoted_path(output, member);
        output << '\n';
    });
    output.flush();
    if (!output) throw std::runtime_error("cannot write results");
    diagnostics << "scanned=" << counters.scanned << " hashed=" << counters.hashed << " cached=" << counters.cached
                << " groups=" << counters.groups << " matches=" << counters.matches
                << " gpu_workers=" << resources.gpu_workers() << " cpu_fallbacks=" << resources.fallbacks() << '\n';
    if (config.backend != "cpu" && resources.gpu_workers() < config.workers)
        diagnostics << "CUDA unavailable or device budget insufficient for some workers; using CPU fallback.\n";
    return 0;
}
}
