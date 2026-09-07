#include "same/compute.hpp"
namespace same {
/// CPU-only 链接占位实现：保留探测接口，明确表示 CUDA 不可用。 / CPU-only link stub preserving
/// probing while reporting CUDA unavailable.
std::unique_ptr<Compute> try_cuda_compute(std::size_t, std::size_t) {
    return {};
}
} // namespace same
