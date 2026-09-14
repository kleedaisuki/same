cmake_minimum_required(VERSION 3.25)
project(same VERSION 0.5.0 LANGUAGES C CXX)

# Native language/target metadata is shared by builds and IDE code models.
# 原生语言及目标元数据同时服务构建与 IDE 代码模型。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SAME_ENABLE_CUDA "Build CUDA backend when a supported compiler is available" ON)
option(SAME_ENABLE_OPENCL "Build optional dynamically loaded iGPU OpenCL backend" ON)
option(SAME_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SAME_REQUIRE_CUDA "Fail configuration unless CUDA is usable" OFF)
include(cmake/Cuda.cmake)
include(CTest)
include(cmake/Dependencies.cmake)

add_library(same_core
  src/config.cpp src/files.cpp src/store.cpp src/compute_cpu.cpp
  src/resources.cpp src/application.cpp src/run_lock.cpp src/terminal.cpp src/walk.cpp src/dispatch.cpp src/online_model.cpp src/model_store.cpp src/telemetry.cpp src/workspace.cpp)
# Propagate the public C++ contract without changing CUDA's separate standard.
# 传播公开 C++ 契约，不改变 CUDA 独立语言标准；头文件集提供 IDE 元数据。
target_compile_features(same_core PUBLIC cxx_std_23)
# Keep executable and persisted run identities aligned. / 程序与持久运行记录共用版本来源。
target_compile_definitions(same_core PUBLIC SAME_VERSION="${PROJECT_VERSION}")
# Fingerprint the dependency's selected SIMD implementation and configurable compiler flags.
# 将依赖实际选择的 SIMD 实现及可配置编译参数加入学习缓存身份。
set(_same_blake3_config "${BLAKE3_SIMD_TYPE}|${BLAKE3_USE_TBB}|${CMAKE_C_COMPILER_ID}|${CMAKE_C_COMPILER_VERSION}|${CMAKE_C_FLAGS}|${CMAKE_C_FLAGS_RELEASE}|${BLAKE3_CFLAGS_SSE2}|${BLAKE3_CFLAGS_SSE4.1}|${BLAKE3_CFLAGS_AVX2}|${BLAKE3_CFLAGS_AVX512}|${BLAKE3_CFLAGS_NEON}")
get_target_property(_same_blake3_definitions blake3 COMPILE_DEFINITIONS)
get_target_property(_same_blake3_options blake3 COMPILE_OPTIONS)
get_target_property(_same_blake3_sources blake3 SOURCES)
string(APPEND _same_blake3_config "|${_same_blake3_definitions}|${_same_blake3_options}|${_same_blake3_sources}|${CMAKE_C_FLAGS_DEBUG}|${CMAKE_C_FLAGS_RELWITHDEBINFO}|${CMAKE_C_FLAGS_MINSIZEREL}")
string(SHA256 _same_blake3_config_hash "${_same_blake3_config}")
target_compile_definitions(same_core PRIVATE SAME_BLAKE3_BUILD_ID="${_same_blake3_config_hash}-$<CONFIG>")

set_target_properties(same_core PROPERTIES CXX_EXTENSIONS OFF)
target_sources(same_core PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
  include/same/application.hpp include/same/compute.hpp include/same/config.hpp
  include/same/files.hpp include/same/resources.hpp include/same/run_lock.hpp
  include/same/store.hpp include/same/terminal.hpp include/same/detail/windows_metadata.hpp
  include/same/detail/windows_path.hpp include/same/walk.hpp include/same/detail/completion_jobs.hpp
  include/same/detail/dispatch.hpp include/same/detail/online_model.hpp include/same/model_store.hpp include/same/telemetry.hpp include/same/workspace.hpp)
target_link_libraries(same_core PUBLIC sqlite3 BLAKE3::blake3 tomlplusplus::tomlplusplus Threads::Threads)
if(WIN32)
  # Include the opt-in in every executable, including tests, for CRT/STL long paths.
  # 所有可执行文件（含测试）均声明支持长路径，覆盖 CRT/STL 文件操作。
  if(MSVC)
    target_sources(same_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/windows.manifest")
  else()
    enable_language(RC)
    target_sources(same_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/windows.rc")
    target_include_directories(same_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  endif()
endif()
if(SAME_CUDA)
  target_sources(same_core PRIVATE src/compute_cuda.cu)
  set_target_properties(same_core PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
else()
  target_sources(same_core PRIVATE src/compute_stub.cpp)
endif()
# No loader is linked: missing runtime remains a supported CPU fallback.
# 不链接 OpenCL 加载器；运行时缺失仍是受支持的 CPU 回退路径。
if(SAME_ENABLE_OPENCL)
  include(cmake/OpenCL.cmake)
  target_sources(same_core PRIVATE src/compute_opencl.cpp)
  target_link_libraries(same_core PRIVATE OpenCL::Headers ${CMAKE_DL_LIBS})
  target_compile_definitions(same_core PRIVATE CL_TARGET_OPENCL_VERSION=120)
else()
  target_sources(same_core PRIVATE src/compute_opencl_stub.cpp)
endif()
if(MSVC)
  # Public headers and tests contain UTF-8 text; preserve their execution encoding.
  # 公开头文件与测试包含 UTF-8 文本，消费者必须使用一致的执行字符集。
  target_compile_options(same_core PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
  target_compile_options(same_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4>)
else()
  target_compile_options(same_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
endif()
if(SAME_SANITIZERS AND NOT MSVC)
  target_compile_options(same_core PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=address,undefined;-fno-omit-frame-pointer>)
  target_link_options(same_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(same src/main.cpp)
target_link_libraries(same PRIVATE same_core)
install(TARGETS same RUNTIME DESTINATION bin)

# 可选基准显式构建，不加入默认程序或测试启动。 / Explicit benchmark target, excluded from default builds/runs.
add_executable(dispatch_benchmark EXCLUDE_FROM_ALL tools/dispatch_benchmark.cpp)
target_link_libraries(dispatch_benchmark PRIVATE same_core)

# Isolated database phases; fused lookup can be compared with the retained legacy API.
# 独立测量数据库阶段；合并查询可与保留的旧 API 公平比较。
add_executable(store_benchmark EXCLUDE_FROM_ALL tools/store_benchmark.cpp)
target_link_libraries(store_benchmark PRIVATE same_core)
target_compile_definitions(store_benchmark PRIVATE SAME_STORE_FUSED_LOOKUP=1)

# Measure routing arithmetic separately from file hashing. / 独立测量路由算术而非文件哈希。
add_executable(online_model_benchmark EXCLUDE_FROM_ALL tools/online_model_benchmark.cpp)
target_link_libraries(online_model_benchmark PRIVATE same_core)
add_executable(online_queue_benchmark EXCLUDE_FROM_ALL tools/online_queue_benchmark.cpp)
target_link_libraries(online_queue_benchmark PRIVATE same_core)
# 固定维度模型热路径测量，不加入默认构建。 / Fixed-dimensional model hot-path microbenchmark.
add_executable(contextual_model_benchmark EXCLUDE_FROM_ALL tools/contextual_model_benchmark.cpp)
target_link_libraries(contextual_model_benchmark PRIVATE same_core)

if(BUILD_TESTING)
  foreach(test igpu_compute igpu_routing compute files config store resources adaptive_routing async_scan run_lock terminal walk completion hash_retry dispatch online_model contextual_model model_store online_resources unified_cuda telemetry workspace)
    add_executable(${test}_tests tests/${test}_tests.cpp)
    target_link_libraries(${test}_tests PRIVATE same_core)
    add_test(NAME ${test} COMMAND ${test}_tests)
    set_tests_properties(${test} PROPERTIES TIMEOUT 120)
  endforeach()
  add_test(NAME build_policy COMMAND "${CMAKE_COMMAND}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_policy.cmake")
  add_executable(integration_tests tests/integration_tests.cpp)
  target_link_libraries(integration_tests PRIVATE same_core)
  if(WIN32)
    target_link_libraries(integration_tests PRIVATE shell32)
  endif()
  add_test(NAME integration COMMAND integration_tests "$<TARGET_FILE:same>")
  set_tests_properties(integration PROPERTIES TIMEOUT 120)
  # 跨进程学习与故障隔离。 / Cross-process learning and failure isolation.
  add_executable(learning_integration_tests tests/learning_integration_tests.cpp)
  target_link_libraries(learning_integration_tests PRIVATE same_core)
  if(WIN32)
    target_link_libraries(learning_integration_tests PRIVATE shell32)
  endif()
  add_test(NAME learning_integration COMMAND learning_integration_tests "$<TARGET_FILE:same>")
  set_tests_properties(learning_integration PROPERTIES TIMEOUT 120)
endif()

include(cmake/Format.cmake)
