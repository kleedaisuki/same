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
    config.workers = config.metadata_workers = 1;
    config.queue_capacity = 8;
    config.block_bytes = config.gpu_min_bytes = 1024;
    config.memory_bytes = config.device_memory_bytes = 64 * 1024 * 1024;
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto gate = release.get_future().share();
    same::Resources resources(config, [&](std::size_t, std::size_t) {
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
            // 工作线程内读取自身计数，不与 prepare_auto 的统计写入竞争。
            // Read counters on their owning worker, without racing prepare_auto statistics.
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
/// 创建仅含路径和载荷大小的输入；hook 不执行文件 I/O。
/// Build metadata-only input; the submission hook performs no file I/O.
same::HashInput input(std::string path, std::uint64_t bytes) {
    same::HashInput result{};
    result.record.path = std::move(path);
    result.record.stamp.size = bytes;
    return result;
}

/// 固定最大候选保留规则，同时覆盖设备缺失与非设备异常的传播。
/// Pin largest-candidate retention and cover missing devices versus non-device exceptions.
void startup_boundary(bool factory_throws) {
    same::Config config;
    config.workers = config.metadata_workers = 1;
    config.queue_capacity = 8;
    config.block_bytes = config.gpu_min_bytes = 1024;
    std::promise<void> entered, release;
    auto started = entered.get_future();
    auto gate = release.get_future().share();
    same::Resources resources(config,
                              [&](std::size_t, std::size_t) -> std::unique_ptr<same::Compute> {
                                  entered.set_value();
                                  gate.wait();
                                  if (factory_throws)
                                      throw std::runtime_error("injected startup boundary failure");
                                  return nullptr;
                              });
    same::AutoHashStartup startup(config, resources);
    std::vector<std::string> submitted;
    auto submit = [&](same::HashInput value, same::detail::HashRoute route) {
        require(route == same::detail::HashRoute::cpu_preferred,
                "uncalibrated eligible input must prefer CPU");
        submitted.push_back(std::move(value.record.path));
    };
    bool released = false;
    try {
        startup.accept(input("smaller", 2048), submit);
        require(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "helper never entered the CUDA factory");
        started.get();
        require(submitted.empty(), "initial candidate was not retained");
        startup.accept(input("largest", 4096), submit);
        require(submitted == std::vector<std::string>{"smaller"},
                "larger candidate must replace and release the smaller input");
        release.set_value();
        released = true;
        bool propagated = false;
        try {
            startup.finish(submit);
        } catch (const std::runtime_error& error) {
            require(std::string(error.what()) == "injected startup boundary failure",
                    "unexpected startup exception");
            propagated = true;
        }
        require(propagated == factory_throws,
                "startup exception propagation differs from contract");
        const auto expected = factory_throws ? std::vector<std::string>{"smaller"}
                                             : std::vector<std::string>{"smaller", "largest"};
        require(submitted == expected, "held input submission differs from startup outcome");
        // get 已消费 future；重复 finish 及后续析构都不能再次发送保留项。
        // get consumed the future; repeated finish and destruction must not resubmit held input.
        startup.finish(submit);
        require(submitted == expected, "finish submitted held input twice");
    } catch (...) {
        // startup 在本 catch 之后析构，先解除工厂门闩再让其 future join。
        // startup destructs after this catch; release its factory before the future joins.
        if (!released)
            release.set_value();
        throw;
    }
}
} // namespace

/// 独立回归入口。 / Standalone regression entry point.
int main() {
    try {
        scan_overlaps_startup();
        startup_boundary(false);
        startup_boundary(true);
        std::cout << "async scan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
