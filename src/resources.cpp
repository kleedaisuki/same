#include "same/resources.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace same {
namespace {
/// 保守缓冲配额与 Config 校验一致。 / Conservative buffer quota matches Config validation.
std::size_t buffer_cost(std::size_t block) {
    return 2 * block + block / 32 + 4096;
}
/// 饱和累加仅用于空闲后的导出。 / Saturating addition used only for idle-time exports.
void add(std::uint64_t& target, std::uint64_t value) {
    target += std::min(value, std::numeric_limits<std::uint64_t>::max() - target);
}
} // namespace

Resources::Resources(const Config& config)
    : Resources(config, try_cuda_compute, try_igpu_compute) {}
Resources::Resources(const Config& config, detail::CudaFactory factory)
    : Resources(config, std::move(factory), detail::CudaFactory{}) {}
Resources::Resources(const Config& config, detail::CudaFactory factory, detail::CudaFactory igpu)
    : pgo_(config.pgo), auto_mode_(config.backend == "auto"), forced_gpu_(config.backend == "cuda"),
      forced_igpu_(config.backend == "igpu"), igpu_factory_(std::move(igpu)),
      capacity_(config.queue_capacity), cuda_factory_(std::move(factory)) {
    config.validate();
        auto remaining = config.memory_bytes - buffer_cost(config.block_bytes) * config.workers;
    if ((auto_mode_ || forced_igpu_) && igpu_factory_) {
        auto block = std::min(config.block_bytes, detail::RoutingParameters::gpu_block_bytes);
        const auto allowance = forced_igpu_ ? remaining : remaining / 2;
        while (block >= 1024 && buffer_cost(block) + 2 * block + 65536 > allowance)
            block /= 2;
        if (block >= 1024) {
            igpu_block_bytes_ = block;
            igpu_budget_ = std::min(config.device_memory_bytes,
                                    std::min(allowance - buffer_cost(block), block + block / 32));
            remaining -= buffer_cost(block) + igpu_budget_;
        }
    }
    gpu_device_budget_ = (config.device_memory_bytes - igpu_budget_) / config.workers;
    const auto share = remaining / config.workers;
    if (forced_gpu_)
        gpu_block_bytes_ = config.block_bytes;
    else if (auto_mode_) {
        auto block = detail::RoutingParameters::gpu_block_bytes;
        while (block >= 1024 && buffer_cost(block) > share)
            block /= 2;
        if (block >= 1024)
            gpu_block_bytes_ = block;
    }
    workers_.reserve(config.workers);
    threads_.reserve(config.workers);
    for (std::size_t i = 0; i < config.workers; ++i) {
        auto worker = std::make_unique<Worker>();
        worker->index = i;
        worker->first.resize(config.block_bytes);
        worker->second.resize(config.block_bytes);
        worker->compute = make_cpu_compute();
        worker->cpu_compute = make_cpu_compute();
        worker->fallbacks = &fallbacks_;
        worker->profile_enabled = pgo_;
        worker->gpu_floor = config.gpu_min_bytes;
        worker->cpu_block_bytes = config.block_bytes;
        worker->gpu_block_bytes = gpu_block_bytes_;
        worker->device_budget_bytes = gpu_device_budget_;
        worker->gpu_reuses_cpu_buffers = forced_gpu_;
        workers_.push_back(std::move(worker));
    }
    try {
        for (auto& worker : workers_)
            threads_.emplace_back([this, ptr = worker.get()] { run(*ptr); });
    } catch (...) {
        close();
        throw;
    }
}
Resources::~Resources() {
    close();
}
void Resources::close() {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    ready_.notify_all();
    space_.notify_all();
    for (auto& thread : threads_)
        if (thread.joinable())
            thread.join();
    igpu_compute_.reset();
}
void Resources::wait_idle() {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
}
void Resources::enqueue(std::function<void(Worker&)> task) {
    std::unique_lock lock(mutex_);
    space_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
    if (closed_)
        throw std::runtime_error("resource manager is closed");
    queue_.push_back(std::move(task));
    ready_.notify_one();
}

std::pair<bool, Resources::Reason> Resources::choose_backend(Worker& worker, std::uint64_t bytes,
                                                             bool hash) {
    if (!hash)
        return {forced_gpu_, Reason::fixed};
    if (!bytes || bytes < worker.gpu_floor)
        return {false, Reason::size};
    if (!auto_mode_)
        return {forced_gpu_, Reason::fixed};
    const bool initial_gpu = bytes >= detail::RoutingParameters::static_gpu_floor_bytes;
    if (!pgo_)
        return {initial_gpu, Reason::fixed};
    const auto band = (std::bit_width(bytes) - 1) / detail::OnlineModel::band_shift;
    const auto arrival = ++worker.arrivals[band];
    if (arrival % detail::RoutingParameters::exploration_period == 0)
        return {(arrival / detail::RoutingParameters::exploration_period) % 2 != 0,
                Reason::explore};
    const auto cpu = worker.model.predict(false, bytes);
    const auto gpu = worker.model.predict(true, bytes);
    if (cpu.known && gpu.known)
        return {detail::gpu_service_wins(cpu, gpu), Reason::model};
    // 两后端各自最多两次冷启动探索；局部机会不被其他线程消耗。
    // At most two initial probes per backend; another worker never consumes local opportunities.
    bool choose_gpu = initial_gpu;
    if (cpu.known && !gpu.known)
        choose_gpu = true;
    else if (!cpu.known && gpu.known)
        choose_gpu = false;
    auto& attempts = choose_gpu ? worker.initial_gpu[band] : worker.initial_cpu[band];
    if (attempts < detail::RoutingParameters::initial_gpu_explorations) {
        ++attempts;
        return {choose_gpu, Reason::explore};
    }
    return {initial_gpu, Reason::fixed};
}

bool Resources::select_igpu(Worker& worker, std::uint64_t bytes, bool hash) {
    if ((!auto_mode_ && !forced_igpu_) || !igpu_block_bytes_ ||
        (hash && (!bytes || bytes < worker.gpu_floor)) || (!hash && !forced_igpu_))
        return false;
    bool want = forced_igpu_;
    if (auto_mode_) {
        const auto igpu = worker.model.predict(BackendKind::igpu, bytes);
        const auto cpu = worker.model.predict(BackendKind::cpu, bytes);
        const auto cuda = worker.model.predict(BackendKind::cuda, bytes);
        // 探索每个工作线程的独立设备模型；无 PGO 时只使用静态轮换。
        // Explore independent owner models; no-PGO uses static rotation only.
        const auto turn = ++worker.igpu_selections;
        const auto band = (std::bit_width(bytes) - 1) / detail::OnlineModel::band_shift;
        want = pgo_ ? ((!igpu.known && worker.initial_igpu[band] < 2 && turn % 3 == 0) ||
                       (turn % detail::RoutingParameters::exploration_period != 1 && igpu.known &&
                        cpu.known && detail::gpu_service_wins(cpu, igpu) &&
                        (!cuda.known || igpu.ms + igpu.error_ms < cuda.ms + cuda.error_ms)) ||
                       turn % detail::RoutingParameters::exploration_period == 0)
                    : bytes >= detail::RoutingParameters::static_gpu_floor_bytes && turn % 2 == 0;
    }
    if (!want)
        return false;
    bool idle = false;
    if (!igpu_busy_.compare_exchange_strong(idle, true, std::memory_order_acquire)) {
        ++worker.igpu_busy;
        return false;
    }
    try {
        if (!igpu_attempted_) {
            igpu_attempted_ = true;
            const auto setup_start = std::chrono::steady_clock::now();
            try {
                igpu_compute_ = igpu_factory_(igpu_block_bytes_, igpu_budget_);
            } catch (...) {
                igpu_setup_ms_ = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - setup_start)
                                     .count();
                throw;
            }
            igpu_setup_ms_ = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - setup_start)
                                 .count();
            if (igpu_compute_) {
                igpu_first_.resize(igpu_block_bytes_);
                igpu_second_.resize(igpu_block_bytes_);
                auto cpu = worker.cpu_compute->hasher();
                auto device = igpu_compute_->hasher();
                cpu->update(igpu_first_);
                device->update(igpu_first_);
                if (cpu->finish() != device->finish()) {
                    igpu_retired_ = true;
                    throw std::runtime_error("iGPU correctness validation mismatch");
                }
            }
        }
        if (!igpu_compute_ || igpu_retired_) {
            igpu_busy_.store(false, std::memory_order_release);
            return false;
        }
        std::swap(worker.compute, igpu_compute_);
        worker.first.swap(igpu_first_);
        worker.second.swap(igpu_second_);
        worker.igpu_selected = true;
        if (pgo_ && hash && bytes) {
            const auto band = (std::bit_width(bytes) - 1) / detail::OnlineModel::band_shift;
            if (worker.initial_igpu[band] < 2)
                ++worker.initial_igpu[band];
        }
        worker.selected_backend = worker.compute.get();
        return true;
    } catch (const ComputeError&) {
        igpu_retired_ = true;
        igpu_compute_.reset();
        igpu_busy_.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        igpu_retired_ = true;
        igpu_compute_.reset();
        igpu_busy_.store(false, std::memory_order_release);
        throw;
    }
}

bool Resources::prepare_gpu(Worker& worker) {
    if (worker.gpu_attempted || worker.gpu_retired)
        return worker.gpu_enabled && !worker.gpu_retired;
    if (!worker.gpu_block_bytes || !worker.device_budget_bytes)
        return false;
    worker.gpu_attempted = true;
    const auto start = std::chrono::steady_clock::now();
    const auto record_time = [&] {
        worker.setup_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    };
    try {
        // 工厂在所属线程调用，每个上下文获得独立 stream；预算绝不争抢。
        // Invoke the factory on the owner thread for an independent stream and noncompeting quota.
        worker.gpu_compute = cuda_factory_(worker.gpu_block_bytes, worker.device_budget_bytes);
        if (!worker.gpu_compute) {
            ++worker.gpu_init_failures;
            record_time();
            return false;
        }
        if (!worker.gpu_reuses_cpu_buffers) {
            worker.gpu_first.resize(worker.gpu_block_bytes);
            worker.gpu_second.resize(worker.gpu_block_bytes);
        }
        auto& buffer = worker.gpu_reuses_cpu_buffers ? worker.first : worker.gpu_first;
        worker.gpu_compute->prepare_input(buffer);
        const auto input = std::span<const std::byte>(buffer).first(
            std::min<std::size_t>(buffer.size(), 64 * 1024));
        auto cpu = worker.cpu_compute->hasher();
        auto gpu = worker.gpu_compute->hasher();
        cpu->update(input);
        gpu->update(input);
        if (cpu->finish() != gpu->finish())
            throw std::runtime_error("GPU correctness validation mismatch");
        worker.gpu_enabled = true;
        gpu_workers_.fetch_add(1, std::memory_order_relaxed);
        record_time();
        return true;
    } catch (const ComputeError&) {
        ++worker.gpu_init_failures;
        worker.gpu_compute.reset();
        record_time();
        return false;
    } catch (...) {
        ++worker.gpu_init_failures;
        worker.gpu_compute.reset();
        record_time();
        throw;
    }
}

void Resources::select_backend(Worker& worker, std::uint64_t bytes, bool hash) noexcept {
    worker.startup_error = {};
    worker.selected_backend = worker.compute.get();
    bool owns_bootstrap = false;
    try {
        if (select_igpu(worker, bytes, hash))
            return;
        const bool eligible = hash && bytes && bytes >= worker.gpu_floor;
        // 不等待冷启动持有者；本次工作继续 CPU，局部探索与设备尝试均保持未消费。
        // Never wait for the cold-start owner: continue on CPU without consuming local GPU
        // attempts.
        if (auto_mode_ && eligible && !worker.gpu_attempted &&
            bootstrap_.load(std::memory_order_acquire) == Bootstrap::initializing) {
            ++worker.cold_start_cpu;
            return;
        }
        const auto band = auto_mode_ && eligible && pgo_
                              ? (std::bit_width(bytes) - 1) / detail::OnlineModel::band_shift
                              : 0;
        const auto initial_gpu = worker.initial_gpu[band];
        const auto [want_gpu, reason] = choose_backend(worker, bytes, hash);
        if (auto_mode_ && want_gpu && !worker.gpu_attempted && !worker.gpu_retired) {
            auto state = Bootstrap::cold;
            owns_bootstrap = bootstrap_.compare_exchange_strong(state, Bootstrap::initializing,
                                                                std::memory_order_acq_rel);
            if (!owns_bootstrap && state == Bootstrap::initializing) {
                worker.initial_gpu[band] = initial_gpu;
                ++worker.cold_start_cpu;
                return;
            }
        }
        const bool gpu = want_gpu && prepare_gpu(worker);
        if (owns_bootstrap)
            bootstrap_.store(Bootstrap::ready, std::memory_order_release);
        if (hash) {
            if (want_gpu && !gpu)
                ++worker.unavailable_cpu;
            else if (reason == Reason::size)
                ++worker.size_cpu;
            else if (reason == Reason::explore)
                ++worker.exploration_jobs;
            else if (reason == Reason::model)
                ++(gpu ? worker.model_gpu : worker.model_cpu);
            else
                ++(gpu ? worker.static_gpu : worker.static_cpu);
        }
        if (!gpu)
            return;
        std::swap(worker.compute, worker.gpu_compute);
        if (!worker.gpu_reuses_cpu_buffers) {
            worker.first.swap(worker.gpu_first);
            worker.second.swap(worker.gpu_second);
        }
        worker.gpu_selected = true;
        worker.selected_backend = worker.compute.get();
        worker.gpu_inflight_at_selection = gpu_active_.fetch_add(1, std::memory_order_relaxed) + 1;
        worker.gpu_peak_concurrency =
            std::max(worker.gpu_peak_concurrency, worker.gpu_inflight_at_selection);
        auto peak = gpu_peak_.load(std::memory_order_relaxed);
        while (peak < worker.gpu_inflight_at_selection &&
               !gpu_peak_.compare_exchange_weak(peak, worker.gpu_inflight_at_selection,
                                                std::memory_order_relaxed)) {
        }
    } catch (...) {
        // 首次失败不应剥夺其他工作线程独立初始化的机会。
        // A first failure must not remove other workers' independent initialization opportunities.
        if (owns_bootstrap)
            bootstrap_.store(Bootstrap::ready, std::memory_order_release);
        worker.startup_error = std::current_exception();
    }
}

void Resources::finish_task(Worker& worker) {
    if (worker.igpu_selected) {
        if (worker.igpu_retired || worker.compute.get() != worker.selected_backend) {
            igpu_retired_ = true;
            worker.compute = std::move(igpu_compute_);
        } else {
            std::swap(worker.compute, igpu_compute_);
        }
        worker.first.swap(igpu_first_);
        worker.second.swap(igpu_second_);
        worker.igpu_selected = false;
        igpu_busy_.store(false, std::memory_order_release);
    }
    if (worker.gpu_selected) {
        if (worker.compute.get() != worker.selected_backend || worker.gpu_retired) {
            worker.compute = std::move(worker.gpu_compute);
            worker.gpu_retired = true;
            worker.gpu_enabled = false;
        } else {
            std::swap(worker.compute, worker.gpu_compute);
        }
        if (!worker.gpu_reuses_cpu_buffers) {
            worker.first.swap(worker.gpu_first);
            worker.second.swap(worker.gpu_second);
        }
        worker.gpu_selected = false;
        gpu_active_.fetch_sub(1, std::memory_order_relaxed);
    } else if (worker.gpu_retired) {
        worker.gpu_compute.reset();
        worker.gpu_enabled = false;
    }
    worker.startup_error = {};
}

void Resources::run(Worker& worker) {
    for (;;) {
        std::function<void(Worker&)> task;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
            if (queue_.empty())
                break;
            task = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        space_.notify_one();
        if (pgo_)
            worker.sample = {};
        const auto retries = worker.fallback_count;
        task(worker);
        // 模型仅在所属线程、队列锁外访问；失败和重试不进入服务统计。
        // Access the model only on its owner, outside the queue lock; exclude failed/retried work.
        if (pgo_ && worker.sample.valid && !worker.startup_error &&
            worker.fallback_count == retries && worker.compute.get() == worker.selected_backend) {
            worker.model.observe(worker.sample.backend == BackendKind::igpu ? BackendKind::igpu
                                 : worker.sample.gpu                        ? BackendKind::cuda
                                                                            : BackendKind::cpu,
                                 worker.sample.bytes, worker.sample.service_ms);
            if (worker.sample.gpu && worker.gpu_inflight_at_selection > 1)
                ++worker.contended_samples;
        }
        finish_task(worker);
        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        space_.notify_all();
    }
    // CUDA 对象和注册缓冲在创建它们的线程销毁，不重置进程共享设备。
    // Destroy CUDA objects/registered buffers on their creating thread; never reset the shared
    // device.
    worker.gpu_compute.reset();
    worker.compute.reset();
    worker.gpu_first.clear();
    worker.gpu_second.clear();
}

bool Resources::gpu_service_enabled() const {
    return std::any_of(workers_.begin(), workers_.end(),
                       [](const auto& w) { return w->gpu_enabled; });
}
std::uint64_t Resources::exploration_jobs() const {
    std::uint64_t total = 0;
    for (const auto& w : workers_)
        add(total, w->exploration_jobs);
    return total;
}
std::pair<std::uint64_t, std::uint64_t> Resources::read_bytes() const {
    std::uint64_t hash = 0, compare = 0;
    for (const auto& w : workers_) {
        add(hash, w->hash_bytes);
        add(compare, w->compare_bytes);
    }
    return {hash, compare};
}
std::pair<std::uint64_t, std::uint64_t> Resources::hash_attempts() const {
    std::uint64_t cpu = 0, gpu = 0;
    for (const auto& w : workers_) {
        add(cpu, w->cpu_hashes);
        add(gpu, w->gpu_hashes);
    }
    return {cpu, gpu};
}
std::uint64_t Resources::igpu_hash_attempts() const {
    std::uint64_t total = 0;
    for (const auto& w : workers_)
        add(total, w->igpu_hashes);
    return total;
}
std::uint64_t Resources::cpu_routed_hashes() const {
    std::uint64_t total = 0;
    for (const auto& w : workers_)
        add(total, w->cpu_routed_hashes);
    return total;
}
detail::OnlineModel::Snapshot Resources::profile_snapshot() const {
    detail::OnlineModel::Snapshot total;
    for (const auto& worker : workers_) {
        const auto local = worker->model.snapshot();
        add(total.samples, local.samples);
        add(total.cpu_samples, local.cpu_samples);
        add(total.gpu_samples, local.gpu_samples);
        add(total.igpu_samples, local.igpu_samples);
        add(total.igpu_known_bands, local.igpu_known_bands);
        add(total.rejected_samples, local.rejected_samples);
        add(total.predicted_samples, local.predicted_samples);
        add(total.cpu_known_bands, local.cpu_known_bands);
        add(total.gpu_known_bands, local.gpu_known_bands);
        for (std::size_t i = 0; i < total.latency_histogram.size(); ++i) {
            add(total.latency_histogram[i], local.latency_histogram[i]);
            add(total.residual_histogram[i], local.residual_histogram[i]);
        }
    }
    return total;
}
std::vector<WorkerProfile> Resources::worker_profiles() const {
    std::vector<WorkerProfile> result;
    result.reserve(workers_.size());
    for (const auto& w : workers_) {
        WorkerProfile p;
        p.index = w->index;
        p.snapshot = w->model.snapshot();
        p.parameters = w->model.parameters();
        p.cpu_block_bytes = w->cpu_block_bytes;
        p.gpu_block_bytes = w->gpu_block_bytes;
        p.device_budget_bytes = w->device_budget_bytes;
        p.setup_ms = w->setup_ms;
        p.gpu_attempted = w->gpu_attempted;
        p.gpu_enabled = w->gpu_enabled;
        p.cpu_hashes = w->cpu_hashes;
        p.gpu_hashes = w->gpu_hashes;
        p.igpu_hashes = w->igpu_hashes;
        p.igpu_hash_bytes = w->igpu_hash_bytes;
        p.igpu_busy = w->igpu_busy;
        p.igpu_selections = w->igpu_selections;
        p.cpu_routed_hashes = w->cpu_routed_hashes;
        p.hash_bytes = w->hash_bytes;
        p.compare_bytes = w->compare_bytes;
        p.cpu_hash_bytes = w->cpu_hash_bytes;
        p.gpu_hash_bytes = w->gpu_hash_bytes;
        p.fallbacks = w->fallback_count;
        p.gpu_init_failures = w->gpu_init_failures;
        p.size_cpu = w->size_cpu;
        p.static_cpu = w->static_cpu;
        p.static_gpu = w->static_gpu;
        p.model_cpu = w->model_cpu;
        p.model_gpu = w->model_gpu;
        p.unavailable_cpu = w->unavailable_cpu;
        p.exploration_jobs = w->exploration_jobs;
        p.cold_start_cpu = w->cold_start_cpu;
        p.gpu_inflight_at_selection = w->gpu_inflight_at_selection;
        p.gpu_peak_concurrency = w->gpu_peak_concurrency;
        p.contended_samples = w->contended_samples;
        result.push_back(std::move(p));
    }
    return result;
}
} // namespace same
