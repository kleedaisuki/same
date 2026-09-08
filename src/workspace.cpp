#include "same/workspace.hpp"
#include "same/config.hpp"
#include "same/run_lock.hpp"
#include <cerrno>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include "same/detail/windows_path.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace same {
namespace {
namespace fs = std::filesystem;
/// 包含 junction 的链接检测；不得沿此路径递归。 / Detect links including junctions; never recurse.
bool linked(const fs::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(detail::windows_path(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return false;
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "workspace attributes");
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}
/// 所有破坏性操作锚定已解析的根目录。 / Anchor destructive operations to a resolved directory.
fs::path checked_root(const fs::path& root) {
    const auto result = fs::canonical(root);
    if (!fs::is_directory(result))
        throw std::runtime_error("workspace root must be a directory");
    return result;
}
/// 状态必须是普通目录，不允许链接跳转。 / State must be an ordinary directory, not a redirect.
bool state_exists(const fs::path& path) {
    if (linked(path))
        throw std::runtime_error("refusing linked .same directory");
    if (!fs::exists(path))
        return false;
    if (!fs::is_directory(path))
        throw std::runtime_error(".same must be a directory");
    return true;
}
/// 保留已有设置；独占运行锁串行化协作写入。 / Preserve existing settings under the run lock.
void write_missing(const fs::path& path, const std::string& contents) {
    if (linked(path) || fs::exists(path)) {
        if (linked(path) || !fs::is_regular_file(path))
            throw std::runtime_error("workspace settings must be regular non-link files");
        return;
    }
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    stream.close();
    if (!stream)
        throw std::runtime_error("cannot write workspace settings");
}
/// 输出真实默认值；预算不是启动时一次性分配量。 / Emit actual defaults, not eager allocations.
std::string default_config() {
    const Config c;
    std::ostringstream out;
    out << "# same 默认配置 / Default configuration\n"
        << "# 字节单位；资源预算不是进程硬上限，也不会一次性全部分配。\n"
        << "# Byte units; budgets are not process hard limits or eager full allocations.\n"
        << "workers = " << c.workers << "\nmetadata_workers = " << c.metadata_workers
        << "\nblock_bytes = " << c.block_bytes
        << "\n# 主机工作缓冲区总预算 / Aggregate host worker-buffer allowance\n"
        << "memory_bytes = " << c.memory_bytes
        << "\n# 工作线程共享显存预算 / Device allowance shared across workers\n"
        << "device_memory_bytes = " << c.device_memory_bytes
        << "\n# 任务数量上限，不是文件内容预读量 / Task-count bound, not payload prefetch\n"
        << "queue_capacity = " << c.queue_capacity << "\ngpu_min_bytes = " << c.gpu_min_bytes
        << "\nbackend = \"" << c.backend << "\"\nrehash = " << (c.rehash ? "true" : "false")
        << "\n# 运行时剖析引导路由，非编译器 PGO；不控制汇总输出 / Runtime profile-guided routing, "
           "not compiler PGO or summary output\n"
        << "pgo = " << (c.pgo ? "true" : "false") << '\n';
    return out.str();
}
/// 保守忽略元数据，不默认隐藏用户文档或构建产物。 / Ignore metadata, not user docs or builds.
const char* default_ignore =
    "# 使用 gitignore 语法；规则相对于工作目录。 / Gitignore syntax, relative to workspace.\n"
    "# .same 始终排除。 / .same is always excluded.\n"
    ".same/\n.git/\n.hg/\n.svn/\n"
    "# 操作系统生成的目录元数据 / OS-generated directory metadata\n"
    ".DS_Store\nThumbs.db\nDesktop.ini\n"
    "# 按需启用，不默认排除可能需要去重的数据。 / Opt in for your own workload.\n"
    "# node_modules/\n# .venv/\n# build/\n";
/// 目录能力持有期间拒绝路径替换；POSIX 子项始终相对于描述符访问。
/// Directory capability prevents Windows replacement; POSIX children are descriptor-relative.
class Node {
public:
    /// 可打印的路径仅供 Windows 在固定祖先下寻址。 / Diagnostic path, Windows anchored addressing.
    fs::path path;
#ifdef _WIN32
    /// 独占写入和删除共享，保证目录祖先不可被替换。 / Deny write/delete sharing to pin ancestry.
    HANDLE handle{INVALID_HANDLE_VALUE};
    /// 打开最终节点本身而非 reparse 目标。 / Open the final node, never its reparse target.
    Node(const fs::path& value, bool erase) : path(value) {
        handle = CreateFileW(detail::windows_path(path).c_str(),
                             FILE_READ_ATTRIBUTES | (erase ? DELETE : 0), FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw std::system_error(GetLastError(), std::system_category(), "open cleanup node");
    }
    /// 释放能力。 / Release capability.
    ~Node() {
        CloseHandle(handle);
    }
    /// 链接永不被当作目录访问。 / Never treat reparse points as traversable directories.
    bool directory() const {
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
            throw std::system_error(GetLastError(), std::system_category(), "cleanup attributes");
        return (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
               !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }
    /// 对已打开的精确对象设置删除，不再次解析路径。 / Delete the exact opened object.
    void erase() {
        FILE_DISPOSITION_INFO info{TRUE};
        if (!SetFileInformationByHandle(handle, FileDispositionInfo, &info, sizeof(info)))
            throw std::system_error(GetLastError(), std::system_category(), "delete cleanup node");
    }
#else
    /// 打开的普通目录，不追踪最终链接。 / Open ordinary directory without following final links.
    int fd{-1};
    Node(int parent, const fs::path& name, const fs::path& value) : path(value) {
        fd = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "open cleanup directory");
    }
    /// 释放描述符。 / Release descriptor.
    ~Node() {
        if (fd >= 0)
            close(fd);
    }
#endif
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    /// 快照当前目录名称，不保存可被重定向的完整路径。 / Snapshot names, not redirectable paths.
    std::vector<fs::path> names() const {
        std::vector<fs::path> result;
#ifdef _WIN32
        for (const auto& entry : fs::directory_iterator(path))
            result.push_back(entry.path().filename());
#else
        const int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (copy < 0)
            throw std::system_error(errno, std::generic_category(), "enumerate cleanup directory");
        DIR* stream = fdopendir(copy);
        if (!stream) {
            close(copy);
            throw std::system_error(errno, std::generic_category());
        }
        errno = 0;
        while (const auto* entry = readdir(stream)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..")
                result.emplace_back(name);
            errno = 0;
        }
        const int error = errno;
        closedir(stream);
        if (error)
            throw std::system_error(error, std::generic_category(), "read cleanup directory");
#endif
        return result;
    }
};
/// 删除子项时始终保留祖先能力；竞态只能失败，不能跟随链接到外部。
/// Retain ancestor capabilities; a replacement race can fail, never redirect traversal outside.
void remove_child(Node& parent, const fs::path& name) {
#ifdef _WIN32
    Node child(parent.path / name, true);
    if (child.directory()) {
        for (const auto& nested : child.names())
            remove_child(child, nested);
    }
    child.erase();
#else
    struct stat info{};
    if (fstatat(parent.fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
        throw std::system_error(errno, std::generic_category(), "inspect cleanup child");
    if (S_ISDIR(info.st_mode)) {
        Node child(parent.fd, name, parent.path / name);
        for (const auto& nested : child.names())
            remove_child(child, nested);
    }
    if (unlinkat(parent.fd, name.c_str(), S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0) != 0)
        throw std::system_error(errno, std::generic_category(), "remove cleanup child");
#endif
}
/// 在同一能力下发现和清除状态；递归时祖先对象不离开作用域。
/// Discover and clean under the same capability; DFS retains every ancestor in scope.
std::size_t clean_directory(Node& directory, bool recursive) {
    std::size_t count = 0;
    for (const auto& name : directory.names()) {
        if (name != ".same")
            continue;
#ifdef _WIN32
        Node state(directory.path / name, true);
        if (!state.directory())
            throw std::runtime_error(".same must be a non-link directory");
        {
            RunLock lock(state.path / "run.lock");
            for (const auto& child : state.names())
                if (child != "run.lock")
                    remove_child(state, child);
        }
        remove_child(state, "run.lock");
        state.erase();
#else
        int locked;
        do {
            locked = flock(directory.fd, LOCK_EX | LOCK_NB);
        } while (locked != 0 && errno == EINTR);
        if (locked != 0)
            throw std::system_error(errno, std::generic_category(), "workspace lifecycle lock");
        Node state(directory.fd, name, directory.path / name);
        RunLock lock(state.fd, "run.lock");
        for (const auto& child : state.names())
            remove_child(state, child);
        if (unlinkat(directory.fd, ".same", AT_REMOVEDIR) != 0)
            throw std::system_error(errno, std::generic_category(), "remove state directory");
#endif
        ++count;
    }
    if (!recursive)
        return count;
    for (const auto& name : directory.names()) {
        if (name == ".same")
            continue;
#ifdef _WIN32
        const auto attributes =
            GetFileAttributesW(detail::windows_path(directory.path / name).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            throw std::system_error(GetLastError(), std::system_category(),
                                    "inspect recursive child");
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        Node child(directory.path / name, false);
        if (child.directory())
            count += clean_directory(child, true);
#else
        struct stat info{};
        if (fstatat(directory.fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
            throw std::system_error(errno, std::generic_category(), "inspect recursive child");
        if (!S_ISDIR(info.st_mode))
            continue;
        Node child(directory.fd, name, directory.path / name);
        count += clean_directory(child, true);
#endif
    }
    return count;
}
} // namespace
void initialize_workspace(const fs::path& root) {
    const auto resolved = checked_root(root);
    WorkspaceLock lifecycle(resolved);
    const auto state = resolved / ".same";
    if (!state_exists(state))
        fs::create_directory(state);
    RunLock lock(state / "run.lock");
    write_missing(state / "config.toml", default_config());
    write_missing(state / "ignore", default_ignore);
}
std::size_t clean_workspace(const fs::path& root, bool recursive) {
    const auto absolute = checked_root(root);
    std::vector<std::unique_ptr<Node>> ancestors;
#ifdef _WIN32
    ancestors.push_back(std::make_unique<Node>(absolute.root_path(), false));
    for (const auto& part : absolute.relative_path()) {
        auto child = std::make_unique<Node>(ancestors.back()->path / part, false);
        if (!child->directory())
            throw std::runtime_error("cleanup ancestor is not an ordinary directory");
        ancestors.push_back(std::move(child));
    }
#else
    ancestors.push_back(
        std::make_unique<Node>(AT_FDCWD, absolute.root_path(), absolute.root_path()));
    for (const auto& part : absolute.relative_path())
        ancestors.push_back(
            std::make_unique<Node>(ancestors.back()->fd, part, ancestors.back()->path / part));
#endif
    return clean_directory(*ancestors.back(), recursive);
}
} // namespace same
