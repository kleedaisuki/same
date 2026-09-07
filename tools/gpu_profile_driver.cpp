/** @file
 * @brief 可指定输入块的完整传输边界微基准。 / Full-transfer-boundary microbenchmark with
 * configurable update blocks. Usage: gpu_profile_driver cpu|gpu bytes block_bytes repeats.
 * 每次摘要都与官方 CPU 比对；初始化、输入生成及判据构造不计时。 / Every digest is checked
 * against official CPU; initialization, input generation, and oracle construction are not timed.
 */
#include "same/compute.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>
/// 首轮预热，之后输出CSV：后端、字节数、块字节数、轮次、微秒。 / Warm up once, then emit
/// CSV: backend, input bytes, block bytes, round, microseconds. Exit nonzero on CUDA/parity error.
int main(int argc, char** argv) {
    const auto size = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1048576ULL;
    const auto block = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1048576ULL;
    const auto repeats = argc > 4 ? std::atoi(argv[4]) : 3;
    if (!block || repeats < 1 ||
        (argc > 1 && std::string(argv[1]) != "cpu" && std::string(argv[1]) != "gpu"))
        return 4;
    auto compute = argc > 1 && std::string(argv[1]) == "cpu"
                       ? same::make_cpu_compute()
                       : same::try_cuda_compute(block, block * 3);
    if (!compute)
        return 2;
    std::vector<std::byte> bytes(size);
    unsigned random = 17;
    for (auto& b : bytes) {
        random = random * 1664525U + 1013904223U;
        b = static_cast<std::byte>(random >> 24);
    }
    auto cpu = same::make_cpu_compute();
    auto oracle = cpu->hasher();
    oracle->update(bytes);
    auto expected = oracle->finish();
    for (int r = 0; r < repeats + 1; ++r) {
        auto start = std::chrono::steady_clock::now();
        auto h = compute->hasher();
        for (std::size_t offset = 0; offset < size; offset += block)
            h->update(
                std::span(bytes).subspan(offset, std::min<std::size_t>(block, size - offset)));
        auto digest = h->finish();
        auto us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                .count();
        if (digest != expected)
            return 3;
        if (r)
            std::cout << compute->name() << ',' << size << ',' << block << ',' << r << ',' << us
                      << '\n';
    }
}
