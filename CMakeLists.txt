cmake_minimum_required(VERSION 3.25)
project(same VERSION 0.1.0 LANGUAGES C CXX)

# Native language/target metadata is shared by builds and IDE code models.
# 原生语言及目标元数据同时服务构建与 IDE 代码模型。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(SAME_ENABLE_CUDA "Build CUDA backend when a supported compiler is available" ON)
option(SAME_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(SAME_REQUIRE_CUDA "Fail configuration unless CUDA is usable" OFF)
include(cmake/Cuda.cmake)
include(CTest)
include(cmake/Dependencies.cmake)

add_library(same_core
  src/config.cpp src/files.cpp src/store.cpp src/compute_cpu.cpp
  src/resources.cpp src/application.cpp src/run_lock.cpp)
# Propagate the public C++ contract without changing CUDA's separate standard.
# 传播公开 C++ 契约，不改变 CUDA 独立语言标准；头文件集提供 IDE 元数据。
target_compile_features(same_core PUBLIC cxx_std_23)
set_target_properties(same_core PROPERTIES CXX_EXTENSIONS OFF)
target_sources(same_core PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
  include/same/application.hpp include/same/compute.hpp include/same/config.hpp
  include/same/files.hpp include/same/resources.hpp include/same/run_lock.hpp
  include/same/store.hpp include/same/detail/windows_metadata.hpp)
target_link_libraries(same_core PUBLIC sqlite3 BLAKE3::blake3 tomlplusplus::tomlplusplus Threads::Threads)
if(SAME_CUDA)
  target_sources(same_core PRIVATE src/compute_cuda.cu)
  set_target_properties(same_core PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
else()
  target_sources(same_core PRIVATE src/compute_stub.cpp)
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

if(BUILD_TESTING)
  foreach(test compute files config store resources run_lock)
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
endif()

include(cmake/Format.cmake)
