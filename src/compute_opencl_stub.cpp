#include "same/compute.hpp"
#include "same/detail/opencl_testing.hpp"
namespace same {
/// 不支持 OpenCL 的构建没有设备资料。 / No device profile without OpenCL support.
std::optional<DeviceProfile> try_igpu_profile(std::size_t, std::size_t) {
    return std::nullopt;
}
/// 未启用 OpenCL 时保持可选工厂。 / Optional factory for builds without OpenCL.
std::unique_ptr<Compute> try_igpu_compute(std::size_t, std::size_t) {
    return {};
}
namespace detail {
std::string opencl_device_name(const Compute&) {
    return {};
}
std::unique_ptr<Compute> try_opencl_test_compute(std::size_t, std::size_t) {
    return {};
}
std::uint64_t opencl_kernel_submissions(const Compute&) {
    return 0;
}
} // namespace detail
} // namespace same
