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
/// 饱和累加仅用于空闲后的导出。 / Saturating addition used only for idle-time exports.
void add(std::uint64_t& target, std::uint64_t value) {
    target += std::min(value, std::numeric_limits<std::uint64_t>::max() - target);
}
} // namespace

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
    : pgo_(config.pgo), auto_mode_(config.backend == "auto"), forced_gpu_(config.backend == "cuda"),
      forced_igpu_(config.backend == "igpu"), setup_loader_(std::move(setup)),
      igpu_probe_(std::move(probe)), igpu_setup_estimate_ms_(config.igpu_bootstrap_ms),
      cold_exploration_fraction_(config.cold_exploration_fraction),
      credit_divisor_(static_cast<double>(config.workers)), prior_loader_(std::move(prior)),
      igpu_factory_(std::move(igpu)), capacity_(config.queue_capacity),
      cuda_factory_(std::move(factory)) {
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
                           : backend_active_[0].load(std::memory_order_relaxed) +
                                 backend_active_[2].load(std::memory_order_relaxed);
    return {bytes, batch, static_cast<double>(peers)};
}

std::pair<BackendKind, Resources::Reason>
Resources::choose_backend(Worker& worker, std::uint64_t bytes, bool hash,
                          const std::array<bool, 3>& candidates) {
    if (hash && (!bytes || bytes < worker.gpu_floor))
        return {BackendKind::cpu, Reason::size};
    if (!auto_mode_ || !hash) {
        const auto forced = forced_gpu_    ? BackendKind::cuda
                            : forced_igpu_ ? BackendKind::igpu
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

bool Resources::discover_igpu(Worker& worker) {
    bool idle = false;
    if (!igpu_busy_.compare_exchange_strong(idle, true, std::memory_order_acquire)) {
        ++worker.igpu_busy;
        return false;
    }
    try {
        if (!igpu_probed_) {
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
                throw;
            }
            igpu_discovery_ms_ =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count();
            if (igpu_present_ && igpu_probe_ && setup_loader_ && pgo_) {
                const auto estimate = setup_loader_(BackendKind::igpu, igpu_profile_);
                if (std::isfinite(estimate) && estimate > 0)
                    igpu_setup_estimate_ms_ = estimate;
            }
        }
        if (igpu_present_ && igpu_probe_)
            load_prior(worker, BackendKind::igpu, igpu_profile_, igpu_block_bytes_);
        const bool present = igpu_present_ && !igpu_retired_;
        igpu_busy_.store(false, std::memory_order_release);
        return present;
    } catch (const ComputeError&) {
        igpu_present_ = false;
        igpu_busy_.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        igpu_busy_.store(false, std::memory_order_release);
        throw;
    }
}

bool Resources::admit_cold_igpu(const Worker& worker) const {
    if (!auto_mode_ || !pgo_)
        return true;
    const auto cpu = worker.model.predict(BackendKind::cpu, worker.candidate_contexts[0]);
    const auto igpu = worker.model.predict(BackendKind::igpu, worker.candidate_contexts[2]);
    const auto saving = detail::RoutingParameters::cpu_advantage_factor * cpu.ms - cpu.error_ms -
                        (igpu.ms + igpu.error_ms);
    if (cpu.known && igpu.known && !cpu.out_of_domain && !igpu.out_of_domain &&
        saving >= igpu_setup_estimate_ms_)
        return true;
    return cold_credit_ms_.load(std::memory_order_relaxed) - cold_spent_ms_ >=
           igpu_setup_estimate_ms_;
}

bool Resources::select_igpu(Worker& worker) {
    bool idle = false;
    if (!igpu_busy_.compare_exchange_strong(idle, true, std::memory_order_acquire)) {
        ++worker.igpu_busy;
        return false;
    }
    bool starting = false;
    std::chrono::steady_clock::time_point setup_start;
    const auto record_setup = [&] {
        if (!starting)
            return;
        igpu_setup_ms_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - setup_start)
                             .count();
        cold_spent_ms_ += igpu_setup_ms_;
        starting = false;
    };
    try {
        if (!igpu_attempted_) {
            if (!admit_cold_igpu(worker)) {
                ++igpu_deferred_;
                igpu_busy_.store(false, std::memory_order_release);
                return false;
            }
            igpu_attempted_ = true;
            starting = true;
            setup_start = std::chrono::steady_clock::now();
            igpu_compute_ = igpu_factory_(igpu_block_bytes_, igpu_budget_);
            if (igpu_compute_) {
                igpu_first_.resize(igpu_block_bytes_);
                igpu_second_.resize(igpu_block_bytes_);
                auto cpu = worker.cpu_compute->hasher();
                auto device = igpu_compute_->hasher();
                cpu->update(igpu_first_);
                device->update(igpu_first_);
                if (cpu->finish() != device->finish())
                    throw std::runtime_error("iGPU correctness validation mismatch");
                auto actual = igpu_compute_->profile();
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
                igpu_profile_ = std::move(actual);
            }
            record_setup();
        }
        if (!igpu_compute_ || igpu_retired_) {
            igpu_busy_.store(false, std::memory_order_release);
            return false;
        }
        load_prior(worker, BackendKind::igpu, igpu_profile_, igpu_block_bytes_);
        std::swap(worker.compute, igpu_compute_);
        worker.first.swap(igpu_first_);
        worker.second.swap(igpu_second_);
        worker.igpu_selected = true;
        worker.selected_backend = worker.compute.get();
        return true;
    } catch (const ComputeError&) {
        record_setup();
        igpu_retired_ = true;
        igpu_compute_.reset();
        igpu_busy_.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        record_setup();
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

bool Resources::activate_cuda(Worker& worker) {
    bool owns_bootstrap = false;
    if (auto_mode_ && !worker.gpu_attempted) {
        auto state = Bootstrap::cold;
        owns_bootstrap = bootstrap_.compare_exchange_strong(state, Bootstrap::initializing,
                                                            std::memory_order_acq_rel);
        if (!owns_bootstrap && state == Bootstrap::initializing) {
            ++worker.cold_start_cpu;
            return false;
        }
    }
    bool available = false;
    try {
        available = prepare_gpu(worker);
    } catch (...) {
        if (owns_bootstrap)
            bootstrap_.store(Bootstrap::ready, std::memory_order_release);
        throw;
    }
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
    worker.startup_error = {};
    worker.selected_backend = worker.compute.get();
    try {
        std::array<bool, 3> candidates{
            true,
            (auto_mode_ || forced_gpu_) && cuda_factory_ && worker.gpu_block_bytes &&
                worker.device_budget_bytes && !worker.gpu_retired &&
                (!worker.gpu_attempted || worker.gpu_enabled),
            (auto_mode_ || forced_igpu_) && igpu_factory_ && igpu_block_bytes_};
        if (candidates[2] &&
            ((hash && bytes && bytes >= worker.gpu_floor) || (!hash && forced_igpu_)))
            candidates[2] = discover_igpu(worker);
        const auto band = bytes ? (std::bit_width(bytes) - 1) / detail::OnlineModel::band_shift : 0;
        if (auto_mode_ && pgo_ && hash && bytes >= worker.gpu_floor && bytes)
            ++worker.arrivals[band];
        // 两设备各至多一次首次资料重选及一次排除；最后必有 CPU。
        // Each device permits one discovery reconsideration and one exclusion; CPU always remains.
        for (unsigned attempt = 0; attempt < 6; ++attempt) {
            if (attempt == 5)
                candidates = {true, false, false};
            for (unsigned slot = 0; slot < 3; ++slot)
                worker.candidate_contexts[slot] =
                    context(worker, static_cast<BackendKind>(slot), bytes);
            const auto [kind, reason] = choose_backend(worker, bytes, hash, candidates);
            const auto slot = static_cast<unsigned>(kind);
            const bool known_device = worker.prior_loaded[slot];
            const bool activated =
                kind == BackendKind::cpu ||
                (kind == BackendKind::cuda ? activate_cuda(worker) : select_igpu(worker));
            if (!activated) {
                candidates[slot] = false;
                ++worker.excluded[slot];
                if (kind == BackendKind::cuda && worker.gpu_attempted)
                    ++worker.unavailable_cpu;
                continue;
            }
            worker.selected_backend = worker.compute.get();
            if (auto_mode_ && pgo_ && !known_device && kind != BackendKind::cpu) {
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
            backend_active_[slot].fetch_add(1, std::memory_order_relaxed);
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
    if (worker.decision_active) {
        backend_active_[static_cast<unsigned>(worker.decision_backend)].fetch_sub(
            1, std::memory_order_relaxed);
        worker.decision_active = false;
    }
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
        if (worker.gpu_counted) {
            gpu_active_.fetch_sub(1, std::memory_order_relaxed);
            worker.gpu_counted = false;
        }
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
            if (worker.sample.bytes == worker.decision_context.bytes) {
                worker.model.observe(worker.decision_backend, worker.decision_context,
                                     worker.sample.service_ms);
                if (worker.decision_backend == BackendKind::cpu && worker.sample.bytes &&
                    worker.sample.bytes >= worker.gpu_floor &&
                    std::isfinite(worker.sample.service_ms) && worker.sample.service_ms > 0)
                    cold_credit_ms_.fetch_add(cold_exploration_fraction_ *
                                                  worker.sample.service_ms / credit_divisor_,
                                              std::memory_order_relaxed);
            }
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
        p.decision_prediction = w->decision_prediction;
        p.decision_backend = w->decision_backend;
        p.decisions = w->decisions;
        p.excluded = w->excluded;
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
