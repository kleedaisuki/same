/** @file
 * @brief 白盒验证元数据句柄移交、计算故障后的完整重试及路径替换防护。
 * White-box regression for metadata-handle handoff, full compute retry, and path replacement.
 * 包含实现仅为测试内部 hash_file，不扩展生产接口；本目标单独链接 same_core。
 * Include the implementation to test internal hash_file without expanding the production API.
 */
#include "../src/application.cpp"
#include <fstream>
#include <iostream>

namespace {
/// 无 NDEBUG 依赖的测试断言。 / Test assertion independent of NDEBUG.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
/// 在收到完整夹具后模拟设备故障，证明首次游标确实从零开始。
/// Fail after receiving the fixture, proving the initial cursor starts at zero.
class FaultHasher final : public same::Hasher {
public:
    /// 检查首批读取内容，然后抛出可重试错误。 / Check first-read bytes, then throw a retryable
    /// error.
    void update(std::span<const std::byte> bytes) override {
        require(bytes.size() == 3 && bytes[0] == std::byte{'a'} && bytes[1] == std::byte{'b'} &&
                    bytes[2] == std::byte{'c'},
                "metadata handoff did not start at offset zero");
        throw same::ComputeError("injected failure after consuming input");
    }
    /// 未读取数据就完成意味着移交游标错误。 / Finishing before reading means a bad handoff cursor.
    same::Digest finish() override {
        throw std::runtime_error("fault hasher unexpectedly reached finish");
    }
};
/// 仅用于注入一次读取后的设备错误；Worker 随后替换为真实 CPU 后端。
/// Inject a post-read device failure; Worker then replaces this with the real CPU backend.
class FaultCompute final : public same::Compute {
public:
    /// 返回故障摘要器。 / Return the fault injector.
    std::unique_ptr<same::Hasher> hasher() override {
        return std::make_unique<FaultHasher>();
    }
    /// 本测试不应调用比较。 / Comparison is not part of this test.
    bool equal(std::span<const std::byte>, std::span<const std::byte>) override {
        throw std::runtime_error("unexpected comparison");
    }
    /// 诊断标签，不假装真实设备。 / Diagnostic label without claiming a real device.
    std::string name() const override {
        return "fault";
    }
};
/// 临时目录独占所有权，异常时也清理。 / Own a temporary fixture and clean on exceptions.
struct Fixture {
    /// 由本测试创建的唯一目录。 / Unique directory created by this test.
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("same-hash-retry-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    /// 建立普通文件内容 abc。 / Create a regular file containing abc.
    Fixture() {
        std::filesystem::create_directory(root);
        std::ofstream out(root / "file", std::ios::binary);
        out << "abc";
        require(static_cast<bool>(out), "fixture write failed");
    }
    /// 清理错误不掩盖断言。 / Cleanup errors must not mask assertions.
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};
/// 验证移交句柄仅首次使用，CPU 重试必须重新打开并重读全部内容。
/// Verify handoff is first-attempt-only and CPU retry reopens and rereads the entire content.
void retry_reopens() {
    Fixture fixture;
    std::atomic<std::size_t> fallbacks{};
    same::Worker worker{};
    worker.first.resize(16);
    worker.compute = std::make_unique<FaultCompute>();
    worker.cpu_compute = same::make_cpu_compute();
    worker.fallbacks = &fallbacks;
    auto opened = std::make_unique<same::FileReader>(fixture.root / "file");
    same::FileRecord record{"file", opened->stamp(), {}};
    auto result = worker.execute(
        [&] { return same::hash_file(fixture.root, record, worker, 0, std::move(opened)); });
    require(!opened, "handoff must transfer ownership");
    require(same::hex_digest(result.digest) ==
                "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
            "retry digest differs from BLAKE3 abc vector");
    require(worker.hash_bytes == 6, "retry must count both complete reads");
    require(fallbacks == 1, "exactly one compute fallback expected");
    require(worker.cpu_routed_hashes == 0, "failure retry is not size-policy routing");
}
/// 已打开句柄仍指向旧文件不能掩盖同一路径被替换。
/// A handle still pointing at the old object must not hide replacement of its path.
void replacement_rejected() {
    Fixture fixture;
    std::atomic<std::size_t> fallbacks{};
    same::Worker worker{};
    worker.first.resize(16);
    worker.compute = same::make_cpu_compute();
    worker.cpu_compute = same::make_cpu_compute();
    worker.fallbacks = &fallbacks;
    auto opened = std::make_unique<same::FileReader>(fixture.root / "file");
    same::FileRecord record{"file", opened->stamp(), {}};
    std::filesystem::rename(fixture.root / "file", fixture.root / "original");
    {
        std::ofstream out(fixture.root / "file", std::ios::binary);
        out << "abc";
        require(static_cast<bool>(out), "replacement write failed");
    }
    bool rejected = false;
    try {
        worker.execute(
            [&] { return same::hash_file(fixture.root, record, worker, 0, std::move(opened)); });
    } catch (const std::runtime_error& error) {
        rejected =
            std::string(error.what()).find("file changed while processing:") != std::string::npos;
    }
    require(rejected, "path replacement was not rejected");
    require(worker.hash_bytes == 0 && fallbacks == 0,
            "path replacement must fail before reading and must not trigger compute fallback");
}
} // namespace
/// 给 CTest 提供明确错误而非无诊断终止。 / Provide actionable CTest errors rather than termination.
int main() {
    try {
        retry_reopens();
        replacement_rejected();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hash retry regression: " << error.what() << '\n';
        return 1;
    }
}
