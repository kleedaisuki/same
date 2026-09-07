#include "same/compute.hpp"
namespace same {
std::unique_ptr<Compute> try_cuda_compute(std::size_t, std::size_t) {
    return {};
}
} // namespace same
