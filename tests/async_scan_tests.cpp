/** @file
 * @brief 验证 GPU 初始化不会阻塞应用的 CPU 文件扫描。 / Verify GPU startup does not stall CPU
 * scanning. 白盒包含应用实现，不扩大生产接口。 / Include application internals without widening
 * production APIs.
 */
#include "../src/application.cpp"
#include <fstream>
#include <iostream>

namespace {
/// 发布构建也保留断言。 / Keep assertions active in release builds.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

/// 模拟设备身份，但保留真实 BLAKE3 算法。 / Fake device identity with real BLAKE3 semantics.
class FakeCuda final : public same::Compute {
public:
    /// 返回真实摘要器。 / Return a real digest implementation.
    std::unique_ptr<same::Hasher> hasher() override {
        return cpu_->hasher();
    }
    /// 保留逐字节比较契约。 / Preserve byte-comparison semantics.
    bool equal(std::span<const std::byte> a, std::span<const std::byte> b) override {
        return cpu_->equal(a, b);
    }
    /// 仅用于硬件无关的设备选择。 / Hardware-independent device selection only.
    std::string name() const override {
        return "cuda";
    }

private:
    /// 实例独占计算状态。 / Instance-owned compute state.
    std::unique_ptr<same::Compute> cpu_{same::make_cpu_compute()};
};

/// 独占测试目录，在数据库和任务退出后清理。 / Own fixtures until database and tasks are closed.
struct Fixture {
    /// 唯一路径避免并行测试冲突。 / Unique path avoids parallel-test collisions.
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("same-async-scan-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    /// 所有文件的期望摘要。 / Expected digest shared by all fixture files.
    same::Digest expected{};
    /// 四个完整文件，内部数据库由 .same 排除。 / Four complete files, excluding the internal
    /// database.
    Fixture() {
        std::filesystem::create_directories(root / ".same");
        std::vector<std::byte> bytes(4096);
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<std::byte>((i * 37 + i / 17) % 256);
        auto hasher = same::make_cpu_compute()->hasher();
        hasher->update(bytes);
        expected = hasher->finish();
        for (int i = 0; i < 4; ++i) {
            std::ofstream out(root / ("file-" + std::to_string(i)), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            require(static_cast<bool>(out), "fixture write failed");
        }
    }
    /// 清理不覆盖原始异常。 / Cleanup must not mask the original exception.
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

/// 门闩解除前至少三个文件应完成；不依赖真实 GPU 或校准胜负。
/// At least three files must finish before releasing startup, independent of GPU/calibration speed.
void scan_overlaps_startup() {
    Fixture fixture;
    same::Config config;
    config.backend = "cuda";
    config.workers = 2;
    config.metadata_workers = 1;
    config.queue_capacity = 8;
    config.block_bytes = 8192;
    config.gpu_min_bytes = 1024;
    config.memory_bytes = config.device_memory_bytes = 128 * 1024 * 1024;
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto gate = release.get_future().share();
    std::atomic<unsigned> factory_calls{};
    same::Resources resources(config,
                              [&](std::size_t, std::size_t) -> std::unique_ptr<same::Compute> {
                                  if (factory_calls.fetch_add(1) != 0)
                                      return nullptr;
                                  entered.set_value();
                                  gate.wait();
                                  return std::make_unique<FakeCuda>();
                              });
    auto scanning = std::async(std::launch::async, [&] {
        same::Store store(fixture.root / ".same" / "state.db");
        same::Counters counters;
        same::scan(fixture.root, config, store, resources, counters, false);
        require(counters.scanned == 4 && counters.hashed == 4, "scan lost fixture files");
        for (int i = 0; i < 4; ++i) {
            const auto record = store.cached("file-" + std::to_string(i));
            require(record && record->stamp.size == 4096 && record->digest == fixture.expected,
                    "scan did not persist a complete BLAKE3 digest");
        }
    });
    // 异常路径也先释放工厂再等待扫描，避免测试自身死锁。
    // Release the factory before joining on every failure path to avoid a test-induced deadlock.
    bool released = false;
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        require(started.wait_until(deadline) == std::future_status::ready,
                "scan never started the CUDA factory");
        started.get();
        std::uint64_t completed = 0;
        while (completed < 3 && std::chrono::steady_clock::now() < deadline) {
            // 工作线程内读取自身计数，不与其他工作线程的局部初始化竞争。
            // Read counters on their owning worker, without racing another worker's local
            // initialization.
            auto observed =
                resources.submit([](same::Worker& worker) { return worker.cpu_hashes; });
            require(observed.wait_until(deadline) == std::future_status::ready,
                    "CPU observer stalled during GPU startup");
            completed = observed.get();
            if (completed < 3)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(completed >= 3, "GPU startup blocked independent CPU file hashing");
        release.set_value();
        released = true;
        scanning.get();
    } catch (...) {
        if (!released)
            release.set_value();
        if (scanning.valid())
            scanning.wait();
        throw;
    }
}
/// 设备缺失只影响所属工作者，所有真实文件仍必须生成完整摘要。
/// A missing device affects only its owner; every real file still gets its complete digest.
void missing_device_scan() {
    Fixture fixture;
    same::Config config;
    config.backend = "cuda";
    config.workers = config.metadata_workers = 1;
    config.queue_capacity = 2;
    config.gpu_min_bytes = 1024;
    config.memory_bytes = config.device_memory_bytes = 128 * 1024 * 1024;
    std::atomic<unsigned> factories{};
    same::Resources resources(config,
                              [&](std::size_t, std::size_t) -> std::unique_ptr<same::Compute> {
                                  ++factories;
                                  return nullptr;
                              });
    same::Store store(fixture.root / ".same" / "state.db");
    same::Counters counters;
    same::scan(fixture.root, config, store, resources, counters, false);
    require(counters.scanned == 4 && counters.hashed == 4, "missing device lost scan input");
    require(factories == 1, "missing device repeatedly initialized on the same worker");
    for (int i = 0; i < 4; ++i) {
        const auto record = store.cached("file-" + std::to_string(i));
        require(record && record->digest == fixture.expected,
                "missing-device scan changed a complete BLAKE3 digest");
    }
    const auto [cpu, gpu] = resources.hash_attempts();
    require(cpu == 4 && gpu == 0, "missing device backend accounting incorrect");
}

/// 空文件不初始化设备，也不消耗后续非空任务的初始化机会。
/// Empty input neither initializes a device nor consumes the later nonempty opportunity.
void empty_before_eligible() {
    Fixture fixture;
    for (int i = 0; i < 4; ++i)
        std::filesystem::remove(fixture.root / ("file-" + std::to_string(i)));
    {
        std::ofstream empty(fixture.root / "empty", std::ios::binary);
    }
    same::Config config;
    config.backend = "cuda";
    config.workers = config.metadata_workers = 1;
    config.gpu_min_bytes = 0;
    config.memory_bytes = config.device_memory_bytes = 128 * 1024 * 1024;
    std::atomic<unsigned> factories{};
    same::Resources resources(config,
                              [&](std::size_t, std::size_t) -> std::unique_ptr<same::Compute> {
                                  ++factories;
                                  return nullptr;
                              });
    same::Store store(fixture.root / ".same" / "state.db");
    same::Counters empty;
    same::scan(fixture.root, config, store, resources, empty, false);
    require(empty.scanned == 1 && empty.hashed == 1 && factories == 0,
            "empty input initialized a device");
    const auto record = store.cached("empty");
    require(record && same::hex_digest(record->digest) ==
                          "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
            "empty scan changed the BLAKE3 empty vector");
    {
        std::ofstream content(fixture.root / "nonempty", std::ios::binary);
        content << "abc";
    }
    same::Counters later;
    same::scan(fixture.root, config, store, resources, later, false);
    require(later.scanned == 2 && later.cached == 1 && later.hashed == 1 && factories == 1,
            "empty input consumed the later device initialization opportunity");
    const auto actual = store.cached("nonempty");
    require(actual && same::hex_digest(actual->digest) ==
                          "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
            "post-empty scan changed the BLAKE3 abc vector");
}
} // namespace

/// 独立回归入口。 / Standalone regression entry point.
int main() {
    try {
        scan_overlaps_startup();
        empty_before_eligible();
        missing_device_scan();
        std::cout << "async scan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
