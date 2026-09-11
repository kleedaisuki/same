# Headers only: drivers are optional and loaded at runtime. / 仅头文件，驱动在运行时按需加载。
# Khronos OpenCL-Headers is Apache-2.0; its license remains in the fetched source.
# Khronos 头文件遵循 Apache-2.0，许可证保留在下载的源码中。
include(FetchContent)
FetchContent_Declare(opencl_headers
  URL https://codeload.github.com/KhronosGroup/OpenCL-Headers/tar.gz/refs/tags/v2025.07.22
  URL_HASH SHA256=98f0a3ea26b4aec051e533cb1750db2998ab8e82eda97269ed6efe66ec94a240
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(OPENCL_HEADERS_BUILD_TESTING OFF CACHE BOOL "Do not build upstream header tests" FORCE)
set(OPENCL_HEADERS_BUILD_CXX_TESTS OFF CACHE BOOL "Do not build upstream C++ tests" FORCE)
FetchContent_MakeAvailable(opencl_headers)
