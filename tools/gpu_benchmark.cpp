/** @file
 * @brief 内存输入 CPU/CUDA 延迟基准，不含初始化和文件 IO。 / In-memory CPU/CUDA latency
 * benchmark excluding initialization and file IO. Build against same_core; optional historical
 * CUDA object with try_cuda_compute renamed to try_cuda_baseline enables paired measurements.
 */
#include "same/compute.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#ifdef SAME_BENCH_BASELINE
namespace same {
std::unique_ptr<Compute> try_cuda_baseline(std::size_t, std::size_t);
}
#endif
/// 计时重复独立文件摘要并消耗输出，避免无用代码消除。 / Time independent digests and consume
/// output to prevent dead-code elimination. Each update uses the production 1 MiB block size.
void run(const char* label, same::Compute& compute) {
    for (auto size : {4096U, 65536U, 1048576U, 16777216U, 67108864U, 268435456U}) {
        std::vector<std::byte> bytes(size, std::byte{37});
        const unsigned repeats = std::max(2U, 16777216U / size);
        unsigned guard = 0;
        for (unsigned round = 0; round < 4; ++round) {
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < repeats; ++i) {
                auto h = compute.hasher();
                for (std::size_t offset = 0; offset < size; offset += 1048576)
                    h->update(std::span(bytes).subspan(
                        offset, std::min<std::size_t>(1048576, size - offset)));
                guard += h->finish()[0];
            }
            const auto seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (round)
                std::cout << label << ',' << size << ',' << repeats << ',' << round << ','
                          << seconds * 1e6 / repeats << ',' << guard << '\n';
        }
    }
}
/// 输出 CSV；CUDA 不可用时失败而非静默测 CPU。 / Emit CSV; fail rather than silently timing
/// CPU when CUDA is unavailable.
int main() {
    auto cpu = same::make_cpu_compute();
    auto gpu = same::try_cuda_compute(1048576, 4194304);
    if (!gpu)
        return 1;
    std::cout << "backend,bytes,repeats,round,microseconds_per_hash,guard\n";
    run("cpu", *cpu);
#ifdef SAME_BENCH_BASELINE
    auto baseline = same::try_cuda_baseline(1048576, 4194304);
    if (!baseline)
        return 1;
    run("baseline", *baseline);
#endif
    run("optimized", *gpu);
}
