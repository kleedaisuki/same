# Hardware-independent route-policy regressions. 无硬件依赖的路线策略回归。
cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/BuildRoutes.cmake")
function(expect_routes mode generator expected)
  same_build_routes(actual "${mode}" "${generator}")
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${mode}/${generator}: expected ${expected}, got ${actual}")
  endif()
endfunction()
expect_routes(auto "Visual Studio 17 2022" "sdk-cuda;msvc-cuda;cpu")
expect_routes(on "Visual Studio 17 2022" "sdk-cuda;msvc-cuda")
expect_routes(off "Visual Studio 17 2022" "cpu")
expect_routes(auto "" "sdk-cuda;cpu")
expect_routes(on "" "sdk-cuda")
expect_routes(off "" "cpu")

# A newer incompatible VS must not erase older candidates before the real compiler probe.
# 较新的不兼容 VS 不得在真实编译探针前抹掉旧候选。
set(_vs_json [=[[
  {"installationPath":"C:/VS/18"},
  {"installationPath":"C:/VS/17"}
]]=])
same_visual_studio_installations(_vs_installations "${_vs_json}")
if(NOT _vs_installations STREQUAL "C:/VS/18;C:/VS/17")
  message(FATAL_ERROR "Visual Studio candidate order was not preserved: ${_vs_installations}")
endif()

# Disabled CUDA must not inspect or reject a generator; conflicting flags fail.
# 禁用 CUDA 时不可探测或拒绝生成器；冲突选项必须失败。
set(SAME_ENABLE_CUDA OFF)
set(CMAKE_GENERATOR "Visual Studio 17 2022")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/Cuda.cmake")
if(SAME_CUDA)
  message(FATAL_ERROR "CPU route unexpectedly enabled CUDA")
endif()
message(STATUS "same build policy checks passed")

# Run policies in separate script processes so failures are observable and isolated.
# 独立脚本进程隔离策略状态，使预期配置失败可直接断言。
function(expect_policy name settings success expected)
  set(_directory "${CMAKE_CURRENT_BINARY_DIR}/build-policy-tests")
  file(MAKE_DIRECTORY "${_directory}")
  set(_script "${_directory}/${name}.cmake")
  file(WRITE "${_script}" "${settings}\ninclude(\"${CMAKE_CURRENT_LIST_DIR}/../cmake/Cuda.cmake\")\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_script}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
  if(success AND NOT _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly failed: ${_stderr}")
  elseif(NOT success AND _result EQUAL 0)
    message(FATAL_ERROR "${name} unexpectedly succeeded")
  endif()
  if(NOT "${_stdout}${_stderr}" MATCHES "${expected}")
    message(FATAL_ERROR "${name}: missing diagnostic ${expected}: ${_stdout}${_stderr}")
  endif()
endfunction()
expect_policy(conflict "set(SAME_ENABLE_CUDA OFF)\nset(SAME_REQUIRE_CUDA ON)" FALSE "conflicts")
expect_policy(mingw "set(WIN32 TRUE)\nset(CMAKE_CXX_COMPILER_ID GNU)\nset(SAME_ENABLE_CUDA ON)" TRUE "requires MSVC")
expect_policy(mingw_strict "set(WIN32 TRUE)\nset(CMAKE_CXX_COMPILER_ID GNU)\nset(SAME_ENABLE_CUDA ON)\nset(SAME_REQUIRE_CUDA ON)" FALSE "requires MSVC")
expect_policy(macos "set(WIN32 FALSE)\nset(APPLE TRUE)\nset(SAME_ENABLE_CUDA ON)" TRUE "not supported on macOS")
