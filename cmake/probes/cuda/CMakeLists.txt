# A real compiler/linker check, not a GPU availability check.
# 实际验证编译器和链接器；不要求存在 GPU，也不下载依赖。
cmake_minimum_required(VERSION 3.25)
project(same_cuda_probe LANGUAGES C CXX)
# The probe enforces the same host ABI/platform contract as the real backend.
# 探测必须遵守实际后端相同的主机 ABI 与平台约束。
if(WIN32 AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  message(FATAL_ERROR "Windows nvcc backend requires MSVC C/C++")
endif()
if(APPLE)
  message(FATAL_ERROR "The CUDA backend is not supported on macOS")
endif()
enable_language(CUDA)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/probe.cu" "#include <cuda_runtime.h>\nint main() { return 0; }\n")
try_compile(SAME_PROBE_WORKS "${CMAKE_CURRENT_BINARY_DIR}/check"
  SOURCES "${CMAKE_CURRENT_BINARY_DIR}/probe.cu" OUTPUT_VARIABLE _output)
if(NOT SAME_PROBE_WORKS)
  message(FATAL_ERROR "CUDA C++20 compilation/link failed: ${_output}")
endif()
