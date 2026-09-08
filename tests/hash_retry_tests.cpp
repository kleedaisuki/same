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

/// 真正消费输入后才注入失败，保留完整摘要算法。 / Inject failure only after real input consumption.
class RoutedHasher final : public same::Hasher {
public:
    /// 摘要器独占算法状态，共享跨线程故障开关。 / Own digest state and share the fault switch.
    RoutedHasher(std::unique_ptr<same::Hasher> inner, std::shared_ptr<std::atomic<bool>> fail)
        : inner_(std::move(inner)), fail_(std::move(fail)) {}
    /// 先消费数据再报错，要求调用者从头重试。 / Consume bytes first, requiring a full restart.
    void update(std::span<const std::byte> bytes) override {
        inner_->update(bytes);
        if (fail_->load())
            throw same::ComputeError("injected routed failure after consuming input");
    }
    /// 未注入故障时返回真实摘要。 / Return the real digest when no fault is injected.
    same::Digest finish() override {
        return inner_->finish();
    }

private:
    /// 每次尝试独立的 BLAKE3 状态。 / Independent BLAKE3 state per attempt.
    std::unique_ptr<same::Hasher> inner_;
    /// 校准结束后启用，避免伪造校准结果。 / Enabled after calibration, never faking its results.
    std::shared_ptr<std::atomic<bool>> fail_;
};

/// 仅设备身份是假的，计算和故障前的数据消费都是真实的。 / Fake identity, real digest and reads.
class RoutedCuda final : public same::Compute {
public:
    /// 接收线程安全的故障控制。 / Accept thread-safe fault control.
    explicit RoutedCuda(std::shared_ptr<std::atomic<bool>> fail) : fail_(std::move(fail)) {}
    /// 包装真实 CPU SIMD 摘要。 / Wrap a real CPU SIMD digest.
    std::unique_ptr<same::Hasher> hasher() override {
        return std::make_unique<RoutedHasher>(cpu_->hasher(), fail_);
    }
    /// 比较也保持真实语义。 / Preserve real comparison semantics too.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 仅用于硬件无关的路由身份。 / Hardware-independent routing identity only.
    std::string name() const override {
        return "cuda";
    }

private:
    /// 工作线程独占的真实实现。 / Real implementation owned by the worker.
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
    /// 校准和文件操作共享故障开关。 / Fault switch shared by calibration and file operations.
    std::shared_ptr<std::atomic<bool>> fail_;
};

/// 异常时自动释放占用任务，任务不引用门闩本体。 / Exception-safe gate with no borrowed task state.
class CpuGate {
public:
    /// 启动通知先于任务发布。 / Create the start notification before publishing the task.
    CpuGate() : started_(state_->started.get_future()) {}
    /// 本对象必须晚于池构造，确保先释放再等待池析构。 / Construct after the pool to release first.
    ~CpuGate() {
        open();
    }
    /// 共享所有权使任务在异常展开期间也安全。 / Shared ownership survives exceptional unwinding.
    auto job() const {
        return [state = state_](same::Worker&) {
            state->started.set_value();
            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&] { return state->open; });
        };
    }
    /// 有界等待仅检测死锁，不根据计时判断速度优势。 / Bounded deadlock check, not a speed test.
    void wait_started() {
        require(started_.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "CPU gate did not start");
    }
    /// 正常和异常路径均可重复释放。 / Idempotent release for normal and exceptional paths.
    void open() {
        {
            std::lock_guard lock(state_->mutex);
            state_->open = true;
        }
        state_->changed.notify_all();
    }

private:
    /// 任务和控制线程共同拥有同步状态。 / Synchronization shared by job and controller.
    struct State {
        /// 保护释放位。 / Protect the release bit.
        std::mutex mutex;
        /// 唤醒阻塞工作者。 / Wake the blocked worker.
        std::condition_variable changed;
        /// 发布确定的 CPU 占用状态。 / Publish definite CPU occupancy.
        std::promise<void> started;
        /// 只从关闭变为打开。 / One-way closed-to-open transition.
        bool open{};
    };
    /// 不依赖栈上门闩的任务状态。 / Task state independent of the stack gate.
    std::shared_ptr<State> state_{std::make_shared<State>()};
    /// 单次启动通知。 / One-shot start notification.
    std::future<void> started_;
};

/// 大 GPU 缓冲不能误把已获资格的小文件送回 CPU；故障必须完整重读。
/// Large GPU buffers must not override file eligibility; a device fault requires a full reread.
void routed_file_hash(bool inject_failure) {
    Fixture fixture;
    std::array<std::byte, 2048> content{};
    for (std::size_t i = 0; i < content.size(); ++i)
        content[i] = static_cast<std::byte>(i % 251);
    {
        std::ofstream out(fixture.root / "file", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(content.data()), content.size());
        require(static_cast<bool>(out), "routed fixture write failed");
    }
    auto reference_cpu = same::make_cpu_compute();
    auto reference = reference_cpu->hasher();
    reference->update(content);
    const auto expected = reference->finish();
    auto opened = std::make_unique<same::FileReader>(fixture.root / "file");
    same::FileRecord record{"file", opened->stamp(), {}};
    same::Config cfg;
    cfg.backend = "auto";
    cfg.workers = 1;
    cfg.queue_capacity = 8;
    cfg.block_bytes = 1024;
    cfg.gpu_min_bytes = 0;
    cfg.gpu_probe_bytes = 0;
    cfg.memory_bytes = 64 * 1024 * 1024;
    auto fail = std::make_shared<std::atomic<bool>>(false);
    same::Resources pool(
        cfg, [fail](std::size_t, std::size_t) { return std::make_unique<RoutedCuda>(fail); });
    pool.prepare_auto(cfg, content.size(), 1);
    pool.wait_idle();
    require(pool.gpu_workers() == 1 && pool.gpu_block_bytes() == 16 * 1024 * 1024,
            "routed fixture requires an independent 16 MiB GPU buffer");
    const auto route = same::detail::classify_hash(
        content.size(), cfg.block_bytes, pool.gpu_block_bytes(), pool.dispatch_evidence());
    require(route == same::detail::HashRoute::cpu_preferred,
            "sub-buffer payload must prefer CPU without excluding GPU overflow");
    fail->store(inject_failure);
    CpuGate cpu;
    auto held = pool.submit(cpu.job());
    cpu.wait_started();
    auto hashed = pool.submit_hash(
        [root = fixture.root, record, opened = std::move(opened)](same::Worker& worker) mutable {
            return worker.execute(
                [&] { return same::hash_file(root, record, worker, 0, std::move(opened)); });
        },
        route);
    require(hashed.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "CPU-saturated file hash did not complete on GPU service");
    require(hashed.get().digest == expected, "routed file digest differs from CPU reference");
    std::future<std::string> queued;
    if (inject_failure)
        queued = pool.submit_hash([](same::Worker& worker) { return worker.compute->name(); },
                                  same::detail::HashRoute::gpu_preferred);
    cpu.open();
    held.get();
    if (inject_failure) {
        require(queued.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "retired GPU stranded subsequent hash work");
        require(queued.get() == "cpu", "retired GPU accepted subsequent hash work");
    }
    pool.wait_idle();
    const auto [cpu_attempts, gpu_attempts] = pool.hash_attempts();
    require(cpu_attempts == (inject_failure ? 1U : 0U) && gpu_attempts == 1,
            "actual file hash attempts disagree with overflow and retry policy");
    require(pool.fallbacks() == (inject_failure ? 1U : 0U),
            "routed compute fault must cause exactly one fallback");
    require(pool.read_bytes().first == content.size() * (inject_failure ? 2U : 1U),
            "routed fault did not reread the entire file");
    require(pool.cpu_routed_hashes() == 0,
            "GPU buffer capacity incorrectly changed file eligibility");
}
} // namespace
/// 给 CTest 提供明确错误而非无诊断终止。 / Provide actionable CTest errors rather than termination.
int main() {
    try {
        retry_reopens();
        replacement_rejected();
        routed_file_hash(false);
        routed_file_hash(true);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hash retry regression: " << error.what() << '\n';
        return 1;
    }
}
