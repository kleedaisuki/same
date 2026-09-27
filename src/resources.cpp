#include "same/resources.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace same {
namespace {
/// 保守缓冲配额与 Config 校验一致。 / Conservative buffer quota matches Config validation.
std::size_t buffer_cost(std::size_t block) {
    return 2 * block + block / 32 + 4096;
}
/// 核显直接复用线程的 CPU 输入缓冲，只新增叶链值暂存。
/// iGPU reuses owner CPU input buffers and adds only leaf-CV host scratch.
std::size_t igpu_host_cost(std::size_t block) {
    return block / 32 + 4096;
}
/// 饱和累加仅用于空闲后的导出。 / Saturating addition used only for idle-time exports.
void add(std::uint64_t& target, std::uint64_t value) {
    target += std::min(value, std::numeric_limits<std::uint64_t>::max() - target);
}
/// 浮点 CAS 兼容尚未提供 fetch_add 的 libc++；饱和避免累计溢出。
/// Floating CAS supports libc++ without fetch_add; saturate cumulative sums.
void add_atomic(std::atomic<double>& target, double value) {
    auto old = target.load(std::memory_order_relaxed);
    while (!target.compare_exchange_weak(
        old, old + std::min(value, std::numeric_limits<double>::max() - old),
        std::memory_order_relaxed)) {
    }
}
} // namespace

Resources::BackendPolicy Resources::parse_backend(std::string_view backend) {
    if (backend == "cpu")
        return BackendPolicy::cpu;
    if (backend == "auto")
        return BackendPolicy::automatic;
    if (backend == "cuda")
        return BackendPolicy::cuda;
    if (backend == "igpu")
        return BackendPolicy::igpu;
    throw std::invalid_argument("unsupported backend policy");
}

Resources::Resources(const Config& config)
    : Resources(config, try_cuda_compute, try_igpu_compute, {}, {}, try_igpu_profile) {}
Resources::Resources(const Config& config, detail::CudaFactory factory)
    : Resources(config, std::move(factory), detail::CudaFactory{}) {}
Resources::Resources(const Config& config, detail::ModelPriorLoader prior,
                     detail::SetupPriorLoader setup)
    : Resources(config, try_cuda_compute, try_igpu_compute, std::move(prior), std::move(setup),
                try_igpu_profile) {}
Resources::Resources(const Config& config, detail::CudaFactory factory, detail::CudaFactory igpu,
                     detail::ModelPriorLoader prior, detail::SetupPriorLoader setup,
                     detail::IgpuProbe probe)
    : pgo_(config.pgo), igpu_bootstrap_ms_(config.igpu_bootstrap_ms),
      cuda_bootstrap_ms_(config.cuda_bootstrap_ms), setup_loader_(std::move(setup)),
      igpu_probe_(std::move(probe)), igpu_setup_estimate_ms_(config.igpu_bootstrap_ms),
      cold_exploration_fraction_(config.cold_exploration_fraction),
      credit_divisor_(static_cast<double>(config.workers)), prior_loader_(std::move(prior)),
      igpu_factory_(std::move(igpu)), capacity_(config.queue_capacity),
      cuda_factory_(std::move(factory)) {
    config.validate();
    policy_ = parse_backend(config.backend);
    auto remaining = config.memory_bytes - buffer_cost(config.block_bytes) * config.workers;
    if ((policy_ == BackendPolicy::automatic || policy_ == BackendPolicy::igpu) && igpu_factory_) {
        auto block = std::min(config.block_bytes, detail::RoutingParameters::gpu_block_bytes);
        const auto allowance =
            (policy_ == BackendPolicy::igpu ? remaining : remaining / 2) / config.workers;
        const auto device_share = config.device_memory_bytes / config.workers;
        while (block >= 1024 && (igpu_host_cost(block) + block + block / 32 + 65536 > allowance ||
                                 block + block / 32 > device_share))
            block /= 2;
        if (block >= 1024) {
            igpu_block_bytes_ = block;
            igpu_budget_ = block + block / 32;
            remaining -= config.workers * (igpu_host_cost(block) + igpu_budget_);
        }
    }
    gpu_device_budget_ =
        (config.device_memory_bytes - config.workers * igpu_budget_) / config.workers;
    const auto share = remaining / config.workers;
    if (policy_ == BackendPolicy::cuda)
        gpu_block_bytes_ = config.block_bytes;
    else if (policy_ == BackendPolicy::automatic) {
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
        worker->igpu_block_bytes = igpu_block_bytes_;
        worker->igpu_budget_bytes = igpu_budget_;
        worker->gpu_reuses_cpu_buffers = policy_ == BackendPolicy::cuda;
        load_prior(*worker, BackendKind::cpu, *worker->compute, config.block_bytes);
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

void Resources::load_prior(Worker& worker, BackendKind kind, const Compute& compute,
                           std::size_t block) {
    load_prior(worker, kind, compute.profile(), block);
}

void Resources::load_prior(Worker& worker, BackendKind kind, const DeviceProfile& profile,
                           std::size_t block) {
    const auto slot = static_cast<unsigned>(kind);
    if (worker.prior_loaded[slot])
        return;
    worker.devices[slot] = profile;
    worker.devices[slot].backend = kind;
    auto& batch = worker.devices[slot].effective_batch_bytes;
    batch = batch ? std::min<std::uint64_t>(batch, block) : block;
    worker.prior_loaded[slot] = true;
    if (pgo_ && prior_loader_) {
        const auto prior = prior_loader_(kind, worker.devices[slot]);
        worker.model.initialize_backend(kind, prior[slot]);
    }
}

detail::OnlineModel::Context Resources::context(const Worker& worker, BackendKind kind,
                                                std::uint64_t bytes) const {
    const auto slot = static_cast<unsigned>(kind);
    auto batch = worker.devices[slot].effective_batch_bytes;
    if (!batch)
        batch = kind == BackendKind::cpu    ? worker.cpu_block_bytes
                : kind == BackendKind::cuda ? worker.gpu_block_bytes
                                            : igpu_block_bytes_;
    // CPU 与 iGPU 共享主机资源，CUDA 仅记录同设备竞争；不是因果带宽估计。
    // CPU/iGPU share host resources; CUDA counts device peers, not causal bandwidth estimates.
    const auto peers = kind == BackendKind::cuda
                           ? backend_active_[1].load(std::memory_order_relaxed)
                           : host_igpu_active_.load(std::memory_order_relaxed);
    return {bytes, batch, static_cast<double>(peers)};
}

std::pair<BackendKind, Resources::Reason>
Resources::choose_backend(Worker& worker, std::uint64_t bytes, bool hash,
                          const std::array<bool, 3>& candidates,
                          std::optional<BackendKind> training_backend) {
    if (training_backend) {
        const auto requested = *training_backend;
        const auto slot = static_cast<unsigned>(requested);
        return {slot < candidates.size() && candidates[slot] ? requested : BackendKind::cpu,
                Reason::fixed};
    }
    if (hash && (!bytes || bytes < worker.gpu_floor))
        return {BackendKind::cpu, Reason::size};
    if (policy_ != BackendPolicy::automatic || !hash) {
        const auto forced = policy_ == BackendPolicy::cuda   ? BackendKind::cuda
                            : policy_ == BackendPolicy::igpu ? BackendKind::igpu
                                                             : BackendKind::cpu;
        return {candidates[static_cast<unsigned>(forced)] ? forced : BackendKind::cpu,
                Reason::fixed};
    }
    const bool large = bytes >= detail::RoutingParameters::static_gpu_floor_bytes;
    const auto preferred = large && candidates[1]   ? BackendKind::cuda
                           : large && candidates[2] ? BackendKind::igpu
                                                    : BackendKind::cpu;
    if (!pgo_)
        return {preferred, Reason::fixed};
    const auto band = (std::bit_width(bytes) - 1) / detail::OnlineModel::band_shift;
    const auto arrival = worker.arrivals[band];
    if (arrival && arrival % detail::RoutingParameters::exploration_period == 0) {
        const auto start = (arrival / detail::RoutingParameters::exploration_period) % 3;
        for (unsigned i = 0; i < 3; ++i) {
            const auto slot = (start + i) % 3;
            if (candidates[slot])
                return {static_cast<BackendKind>(slot), Reason::explore};
        }
    }
    std::array<detail::OnlineModel::Prediction, 3> estimates{};
    for (unsigned slot = 0; slot < 3; ++slot)
        if (candidates[slot]) {
            const auto kind = static_cast<BackendKind>(slot);
            estimates[slot] = worker.model.predict(kind, worker.candidate_contexts[slot]);
        }
    const std::array<unsigned, 3> order =
        large ? std::array<unsigned, 3>{1, 0, 2} : std::array<unsigned, 3>{0, 1, 2};
    const std::array<unsigned, 3> attempts{worker.initial_cpu[band], worker.initial_gpu[band],
                                           worker.initial_igpu[band]};
    unsigned least_probes = detail::RoutingParameters::initial_gpu_explorations;
    unsigned probe_slot = 3;
    for (auto slot : order)
        if (candidates[slot] && (!estimates[slot].known || estimates[slot].out_of_domain) &&
            attempts[slot] < least_probes) {
            least_probes = attempts[slot];
            probe_slot = slot;
        }
    if (probe_slot < 3)
        return {static_cast<BackendKind>(probe_slot), Reason::explore};
    auto best = BackendKind::cpu;
    bool known = estimates[0].known && !estimates[0].out_of_domain;
    double score = known ? detail::RoutingParameters::cpu_advantage_factor * estimates[0].ms -
                               estimates[0].error_ms
                         : std::numeric_limits<double>::infinity();
    for (unsigned slot = 1; slot < 3; ++slot) {
        const auto& prediction = estimates[slot];
        if (candidates[slot] && prediction.known && !prediction.out_of_domain &&
            prediction.ms + prediction.error_ms < score) {
            score = prediction.ms + prediction.error_ms;
            best = static_cast<BackendKind>(slot);
            known = true;
        }
    }
    return known ? std::pair{best, Reason::model} : std::pair{preferred, Reason::fixed};
}

bool Resources::begin_cold(const Worker& worker, double estimate, bool training) {
    if (training || policy_ != BackendPolicy::automatic || !pgo_)
        return true;
    bool idle = false;
    if (!cold_start_busy_.compare_exchange_strong(idle, true, std::memory_order_acquire))
        return false;
    const auto cpu = worker.model.predict(BackendKind::cpu, worker.candidate_contexts[0]);
    const bool potential =
        cpu.known && !cpu.out_of_domain &&
        detail::RoutingParameters::cpu_advantage_factor * cpu.ms - cpu.error_ms >= estimate;
    const bool funded = cold_credit_ms_.load(std::memory_order_relaxed) -
                            cold_spent_ms_.load(std::memory_order_relaxed) >=
                        estimate;
    if (estimate == 0 || potential || funded)
        return true;
    cold_start_busy_.store(false, std::memory_order_release);
    return false;
}

void Resources::end_cold() {
    if (policy_ == BackendPolicy::automatic && pgo_)
        cold_start_busy_.store(false, std::memory_order_release);
}

bool Resources::discover_igpu(Worker& worker, bool training) {
    const bool required = training || policy_ == BackendPolicy::igpu;
    bool idle = false;
    while (!igpu_probe_busy_.compare_exchange_strong(idle, true, std::memory_order_acquire)) {
        if (!required) {
            ++worker.igpu_busy;
            return false;
        }
        igpu_probe_busy_.wait(true, std::memory_order_acquire);
        idle = false;
    }
    const auto release_probe = [&] {
        igpu_probe_busy_.store(false, std::memory_order_release);
        igpu_probe_busy_.notify_all();
    };
    try {
        if (!igpu_probed_) {
            if (!begin_cold(worker, igpu_bootstrap_ms_, training)) {
                ++igpu_deferred_;
                release_probe();
                return false;
            }
            igpu_probed_ = true;
            const auto start = std::chrono::steady_clock::now();
            try {
                if (igpu_probe_) {
                    const auto profile = igpu_probe_(igpu_block_bytes_, igpu_budget_);
                    igpu_present_ = profile.has_value();
                    if (profile) {
                        igpu_profile_ = *profile;
                        igpu_profile_.backend = BackendKind::igpu;
                        auto& batch = igpu_profile_.effective_batch_bytes;
                        batch = batch ? std::min<std::uint64_t>(batch, igpu_block_bytes_)
                                      : igpu_block_bytes_;
                    }
                }
            } catch (...) {
                igpu_present_ = false;
                igpu_discovery_ms_ = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                add_atomic(cold_spent_ms_, igpu_discovery_ms_);
                if (!training)
                    end_cold();
                throw;
            }
            igpu_discovery_ms_ =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            add_atomic(cold_spent_ms_, igpu_discovery_ms_);
            if (!training)
                end_cold();
            if (igpu_present_ && igpu_probe_ && setup_loader_ && pgo_) {
                const auto estimate = setup_loader_(BackendKind::igpu, igpu_profile_);
                if (std::isfinite(estimate) && estimate > 0)
                    igpu_setup_estimate_ms_ = estimate;
            }
        }
        if (igpu_present_ && igpu_probe_)
            load_prior(worker, BackendKind::igpu, igpu_profile_, igpu_block_bytes_);
        const bool present = igpu_present_;
        release_probe();
        return present;
    } catch (const ComputeError&) {
        igpu_present_ = false;
        release_probe();
        return false;
    } catch (...) {
        release_probe();
        throw;
    }
}

bool Resources::admit_cold_igpu(const Worker& worker, bool training) const {
    if (training || policy_ != BackendPolicy::automatic || !pgo_ || igpu_setup_estimate_ms_ == 0)
        return true;
    const auto cpu = worker.model.predict(BackendKind::cpu, worker.candidate_contexts[0]);
    const auto igpu = worker.model.predict(BackendKind::igpu, worker.candidate_contexts[2]);
    const auto saving = detail::RoutingParameters::cpu_advantage_factor * cpu.ms - cpu.error_ms -
                        (igpu.ms + igpu.error_ms);
    if (cpu.known && igpu.known && !cpu.out_of_domain && !igpu.out_of_domain &&
        saving >= igpu_setup_estimate_ms_)
        return true;
    return cold_credit_ms_.load(std::memory_order_relaxed) -
               cold_spent_ms_.load(std::memory_order_relaxed) >=
           igpu_setup_estimate_ms_;
}

bool Resources::select_igpu(Worker& worker, bool training) {
    if (worker.igpu_retired)
        return false;
    const bool starting = !worker.igpu_attempted;
    const bool cold = starting && !igpu_ready_.load(std::memory_order_acquire);
    const bool guarded = cold && !training && policy_ == BackendPolicy::automatic && pgo_;
    if (guarded && !begin_cold(worker, igpu_setup_estimate_ms_, false)) {
        ++igpu_deferred_;
        return false;
    }
    if (cold && !admit_cold_igpu(worker, training)) {
        if (guarded)
            end_cold();
        ++igpu_deferred_;
        return false;
    }
    const auto setup_start = std::chrono::steady_clock::now();
    const auto record_setup = [&] {
        if (starting) {
            const auto elapsed = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - setup_start)
                                     .count();
            worker.igpu_setup_ms += elapsed;
            add_atomic(cold_spent_ms_, elapsed);
        }
        if (guarded)
            end_cold();
    };
    try {
        if (starting) {
            worker.igpu_attempted = true;
            worker.igpu_compute = igpu_factory_(igpu_block_bytes_, igpu_budget_);
            if (worker.igpu_compute) {
                auto cpu = worker.cpu_compute->hasher();
                auto device = worker.igpu_compute->hasher();
                const auto input =
                    std::span(worker.first)
                        .first(std::min<std::size_t>(worker.first.size(), 64 * 1024));
                cpu->update(input);
                device->update(input);
                if (cpu->finish() != device->finish())
                    throw std::runtime_error("iGPU correctness validation mismatch");
                auto actual = worker.igpu_compute->profile();
                actual.backend = BackendKind::igpu;
                auto& batch = actual.effective_batch_bytes;
                batch =
                    batch ? std::min<std::uint64_t>(batch, igpu_block_bytes_) : igpu_block_bytes_;
                const auto identity = [](const DeviceProfile& p) {
                    return std::tie(p.backend, p.device_name, p.vendor, p.driver_version,
                                    p.implementation_version, p.architecture, p.compute_units,
                                    p.hardware_threads, p.global_memory_bytes,
                                    p.max_allocation_bytes, p.effective_batch_bytes,
                                    p.unified_memory);
                };
                if (igpu_probe_ && identity(actual) != identity(igpu_profile_))
                    throw std::runtime_error(
                        "iGPU profile changed between discovery and activation");
                load_prior(worker, BackendKind::igpu, actual, igpu_block_bytes_);
                worker.igpu_enabled = true;
                igpu_ready_.store(true, std::memory_order_release);
            } else {
                worker.igpu_retired = true;
            }
            record_setup();
        }
        if (!worker.igpu_enabled || !worker.igpu_compute)
            return false;
        std::swap(worker.compute, worker.igpu_compute);
        worker.igpu_selected = true;
        worker.selected_backend = worker.compute.get();
        return true;
    } catch (const ComputeError&) {
        record_setup();
        worker.igpu_retired = true;
        worker.igpu_enabled = false;
        worker.igpu_compute.reset();
        return false;
    } catch (...) {
        record_setup();
        worker.igpu_retired = true;
        worker.igpu_enabled = false;
        worker.igpu_compute.reset();
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
        load_prior(worker, BackendKind::cuda, *worker.gpu_compute, worker.gpu_block_bytes);
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

bool Resources::activate_cuda(Worker& worker, bool training) {
    bool owns_bootstrap = false;
    if (policy_ == BackendPolicy::automatic && !worker.gpu_attempted && !training) {
        auto state = Bootstrap::cold;
        owns_bootstrap = bootstrap_.compare_exchange_strong(state, Bootstrap::initializing,
                                                            std::memory_order_acq_rel);
        if (!owns_bootstrap && state == Bootstrap::initializing) {
            ++worker.cold_start_cpu;
            return false;
        }
    }
    const bool starting = !worker.gpu_attempted;
    const bool guarded = starting && !training && policy_ == BackendPolicy::automatic && pgo_;
    if (starting && !begin_cold(worker, cuda_bootstrap_ms_, training)) {
        ++cuda_deferred_;
        if (owns_bootstrap)
            bootstrap_.store(Bootstrap::cold, std::memory_order_release);
        return false;
    }
    const auto setup_before = worker.setup_ms;
    const auto finish_setup = [&] {
        if (!starting)
            return;
        add_atomic(cold_spent_ms_, worker.setup_ms - setup_before);
        if (guarded)
            end_cold();
    };
    bool available = false;
    try {
        available = prepare_gpu(worker);
    } catch (...) {
        finish_setup();
        if (owns_bootstrap)
            bootstrap_.store(Bootstrap::ready, std::memory_order_release);
        throw;
    }
    finish_setup();
    if (owns_bootstrap)
        bootstrap_.store(Bootstrap::ready, std::memory_order_release);
    if (!available)
        return false;
    std::swap(worker.compute, worker.gpu_compute);
    if (!worker.gpu_reuses_cpu_buffers) {
        worker.first.swap(worker.gpu_first);
        worker.second.swap(worker.gpu_second);
    }
    worker.gpu_selected = true;
    return true;
}

void Resources::select_backend(Worker& worker, std::uint64_t bytes, bool hash) noexcept {
    select_backend(worker, bytes, hash, std::nullopt);
}

void Resources::select_backend(Worker& worker, std::uint64_t bytes, bool hash,
                               std::optional<BackendKind> training_backend) noexcept {
    worker.startup_error = {};
    worker.selected_backend = worker.compute.get();
    try {
        std::array<bool, 3> candidates{
            true,
            (policy_ == BackendPolicy::automatic || policy_ == BackendPolicy::cuda) &&
                cuda_factory_ && worker.gpu_block_bytes && worker.device_budget_bytes &&
                !worker.gpu_retired && (!worker.gpu_attempted || worker.gpu_enabled),
            (policy_ == BackendPolicy::automatic || policy_ == BackendPolicy::igpu) &&
                igpu_factory_ && worker.igpu_block_bytes && worker.igpu_budget_bytes &&
                !worker.igpu_retired && (!worker.igpu_attempted || worker.igpu_enabled)};
        const auto band = bytes ? (std::bit_width(bytes) - 1) / detail::OnlineModel::band_shift : 0;
        if (policy_ == BackendPolicy::automatic && pgo_ && hash && bytes >= worker.gpu_floor &&
            bytes)
            ++worker.arrivals[band];
        // 两设备各至多一次首次资料重选及一次排除；最后必有 CPU。
        // Each device permits one discovery reconsideration and one exclusion; CPU always remains.
        for (unsigned attempt = 0; attempt < 6; ++attempt) {
            if (attempt == 5)
                candidates = {true, false, false};
            for (unsigned slot = 0; slot < 3; ++slot)
                worker.candidate_contexts[slot] =
                    context(worker, static_cast<BackendKind>(slot), bytes);
            const auto [kind, reason] =
                choose_backend(worker, bytes, hash, candidates, training_backend);
            const auto slot = static_cast<unsigned>(kind);
            const bool known_device = worker.prior_loaded[slot];
            if (kind == BackendKind::igpu && igpu_probe_ && !known_device) {
                if (!discover_igpu(worker, training_backend.has_value())) {
                    candidates[slot] = false;
                    ++worker.excluded[slot];
                }
                continue;
            }
            const bool activated =
                kind == BackendKind::cpu ||
                (kind == BackendKind::cuda ? activate_cuda(worker, training_backend.has_value())
                                           : select_igpu(worker, training_backend.has_value()));
            if (!activated) {
                candidates[slot] = false;
                ++worker.excluded[slot];
                if (kind == BackendKind::cuda && worker.gpu_attempted)
                    ++worker.unavailable_cpu;
                continue;
            }
            worker.selected_backend = worker.compute.get();
            if (policy_ == BackendPolicy::automatic && pgo_ && !known_device &&
                kind != BackendKind::cpu) {
                // 首次发现的真实容量与先验必须参与执行前的同一选择过程。
                // Newly discovered capacity/prior participates before execution, not after it.
                finish_task(worker);
                continue;
            }
            worker.selected_backend = worker.compute.get();
            worker.decision_backend = kind;
            worker.decision_context = worker.candidate_contexts[slot];
            // 首次初始化才知道实际批容量；冻结并记录该执行形状的预测。
            // First setup reveals actual capacity; freeze and predict this execution shape.
            worker.decision_context.effective_batch_bytes =
                worker.devices[slot].effective_batch_bytes;
            worker.decision_prediction = pgo_ ? worker.model.predict(kind, worker.decision_context)
                                              : detail::OnlineModel::Prediction{};
            if (kind == BackendKind::cuda) {
                worker.contention_epoch = cuda_overlap_epoch_.load(std::memory_order_relaxed);
                worker.contention_at_start =
                    backend_active_[slot].fetch_add(1, std::memory_order_relaxed);
                if (worker.contention_at_start)
                    cuda_overlap_epoch_.fetch_add(1, std::memory_order_relaxed);
            } else {
                worker.contention_epoch = host_igpu_overlap_epoch_.load(std::memory_order_relaxed);
                worker.contention_at_start =
                    host_igpu_active_.fetch_add(1, std::memory_order_relaxed);
                if (worker.contention_at_start)
                    host_igpu_overlap_epoch_.fetch_add(1, std::memory_order_relaxed);
                backend_active_[slot].fetch_add(1, std::memory_order_relaxed);
            }
            worker.decision_active = true;
            if (kind == BackendKind::cuda) {
                worker.gpu_counted = true;
                worker.gpu_inflight_at_selection =
                    gpu_active_.fetch_add(1, std::memory_order_relaxed) + 1;
                worker.gpu_peak_concurrency =
                    std::max(worker.gpu_peak_concurrency, worker.gpu_inflight_at_selection);
                auto peak = gpu_peak_.load(std::memory_order_relaxed);
                while (peak < worker.gpu_inflight_at_selection &&
                       !gpu_peak_.compare_exchange_weak(peak, worker.gpu_inflight_at_selection,
                                                        std::memory_order_relaxed)) {
                }
            }
            if (kind == BackendKind::igpu) {
                const auto active = igpu_active_.fetch_add(1, std::memory_order_relaxed) + 1;
                auto peak = igpu_peak_.load(std::memory_order_relaxed);
                while (peak < active &&
                       !igpu_peak_.compare_exchange_weak(peak, active, std::memory_order_relaxed)) {
                }
            }
            if (!hash)
                return;
            const auto reason_slot = reason == Reason::explore ? 2
                                     : reason == Reason::model ? 1
                                                               : 0;
            ++worker.decisions[slot][reason_slot];
            if (reason == Reason::explore) {
                ++worker.exploration_jobs;
                auto& probes = kind == BackendKind::cpu    ? worker.initial_cpu[band]
                               : kind == BackendKind::cuda ? worker.initial_gpu[band]
                                                           : worker.initial_igpu[band];
                if (probes < detail::RoutingParameters::initial_gpu_explorations)
                    ++probes;
            } else if (reason == Reason::size)
                ++worker.size_cpu;
            else if (reason == Reason::model && kind != BackendKind::igpu)
                ++(kind == BackendKind::cuda ? worker.model_gpu : worker.model_cpu);
            else if (reason == Reason::fixed && kind != BackendKind::igpu)
                ++(kind == BackendKind::cuda ? worker.static_gpu : worker.static_cpu);
            if (kind == BackendKind::igpu)
                ++worker.igpu_selections;
            return;
        }
    } catch (...) {
        worker.startup_error = std::current_exception();
    }
}

void Resources::finish_task(Worker& worker) {
    const bool counted_igpu =
        worker.decision_active && worker.decision_backend == BackendKind::igpu;
    if (worker.decision_active) {
        if (worker.decision_backend != BackendKind::cuda)
            host_igpu_active_.fetch_sub(1, std::memory_order_relaxed);
        backend_active_[static_cast<unsigned>(worker.decision_backend)].fetch_sub(
            1, std::memory_order_relaxed);
        worker.decision_active = false;
    }
    if (worker.igpu_selected) {
        if (counted_igpu)
            igpu_active_.fetch_sub(1, std::memory_order_relaxed);
        if (worker.igpu_retired || worker.compute.get() != worker.selected_backend) {
            worker.compute = std::move(worker.igpu_compute);
            worker.igpu_retired = true;
            worker.igpu_enabled = false;
        } else {
            std::swap(worker.compute, worker.igpu_compute);
        }
        worker.igpu_selected = false;
    } else if (worker.igpu_retired)
        worker.igpu_compute.reset();
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
    } else if (worker.gpu_retired) {
        worker.gpu_compute.reset();
        worker.gpu_enabled = false;
    }
    if (worker.gpu_counted) {
        gpu_active_.fetch_sub(1, std::memory_order_relaxed);
        worker.gpu_counted = false;
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
            if (worker.sample.bytes == worker.decision_context.bytes) {
                worker.learning_context = worker.decision_context;
                const auto epoch = worker.decision_backend == BackendKind::cuda
                                       ? cuda_overlap_epoch_.load(std::memory_order_relaxed)
                                       : host_igpu_overlap_epoch_.load(std::memory_order_relaxed);
                worker.learning_context.contention =
                    std::max({worker.learning_context.contention,
                              static_cast<double>(worker.contention_at_start),
                              epoch != worker.contention_epoch ? 1.0 : 0.0});
                worker.model.observe(worker.decision_backend, worker.learning_context,
                                     worker.sample.service_ms, worker.decision_prediction);
                if (worker.decision_backend == BackendKind::cpu && worker.sample.bytes &&
                    worker.sample.bytes >= worker.gpu_floor &&
                    std::isfinite(worker.sample.service_ms) && worker.sample.service_ms > 0)
                    add_atomic(cold_credit_ms_, cold_exploration_fraction_ *
                                                    worker.sample.service_ms / credit_divisor_);
            }
            if (worker.decision_backend != BackendKind::cpu &&
                worker.learning_context.contention > 0) {
                if (worker.decision_backend == BackendKind::cuda)
                    ++worker.contended_samples;
                else if (worker.decision_backend == BackendKind::igpu)
                    ++worker.igpu_contended_samples;
            }
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
    worker.igpu_compute.reset();
    worker.compute.reset();
    worker.gpu_first.clear();
    worker.gpu_second.clear();
}

bool Resources::gpu_service_enabled() const {
    return std::any_of(workers_.begin(), workers_.end(),
                       [](const auto& w) { return w->gpu_enabled; });
}
bool Resources::igpu_service_enabled() const {
    return std::any_of(workers_.begin(), workers_.end(),
                       [](const auto& w) { return w->igpu_enabled; });
}
bool Resources::igpu_attempted() const {
    return std::any_of(workers_.begin(), workers_.end(),
                       [](const auto& w) { return w->igpu_attempted; });
}
bool Resources::igpu_retired() const {
    return !igpu_service_enabled() && std::any_of(workers_.begin(), workers_.end(),
                                                  [](const auto& w) { return w->igpu_retired; });
}
double Resources::igpu_setup_ms() const {
    double total = 0;
    for (const auto& w : workers_)
        total += w->igpu_setup_ms;
    return total;
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
        add(total.out_of_domain_samples, local.out_of_domain_samples);
        add(total.numerical_rejections, local.numerical_rejections);
        total.absolute_error_sum_ms +=
            std::min(local.absolute_error_sum_ms,
                     std::numeric_limits<double>::max() - total.absolute_error_sum_ms);
        total.squared_error_sum_ms2 +=
            std::min(local.squared_error_sum_ms2,
                     std::numeric_limits<double>::max() - total.squared_error_sum_ms2);
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
        p.prior = w->model.prior();
        p.delta = w->model.delta();
        p.devices = w->devices;
        p.decision_context = w->decision_context;
        p.learning_context = w->learning_context;
        p.decision_prediction = w->decision_prediction;
        p.decision_backend = w->decision_backend;
        p.decisions = w->decisions;
        p.excluded = w->excluded;
        p.cpu_block_bytes = w->cpu_block_bytes;
        p.gpu_block_bytes = w->gpu_block_bytes;
        p.device_budget_bytes = w->device_budget_bytes;
        p.igpu_block_bytes = w->igpu_block_bytes;
        p.igpu_budget_bytes = w->igpu_budget_bytes;
        p.setup_ms = w->setup_ms;
        p.igpu_setup_ms = w->igpu_setup_ms;
        p.gpu_attempted = w->gpu_attempted;
        p.gpu_enabled = w->gpu_enabled;
        p.igpu_attempted = w->igpu_attempted;
        p.igpu_enabled = w->igpu_enabled;
        p.cpu_hashes = w->cpu_hashes;
        p.gpu_hashes = w->gpu_hashes;
        p.igpu_hashes = w->igpu_hashes;
        p.igpu_hash_bytes = w->igpu_hash_bytes;
        p.igpu_busy = w->igpu_busy;
        p.igpu_selections = w->igpu_selections;
        p.igpu_contended_samples = w->igpu_contended_samples;
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
