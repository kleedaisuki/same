#pragma once
#include "same/compute.hpp"
namespace same::detail {
/// 仅测试使用，允许 CPU OpenCL 实现；生产选择不受影响。 / Test-only CPU-ICD opt-in; production
/// unaffected.
std::unique_ptr<Compute> try_opencl_test_compute(std::size_t block_bytes,
                                                 std::size_t device_budget);
/// 驱动设备名，非 OpenCL 返回空。 / Driver device name; empty for other backends.
std::string opencl_device_name(const Compute& compute);
/// 已完成的内核批次，不含自检。 / Completed kernel batches excluding startup validation.
std::uint64_t opencl_kernel_submissions(const Compute& compute);
} // namespace same::detail
