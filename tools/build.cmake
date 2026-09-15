# Native Windows launcher; configure with -D options before -P.
# Windows 原生入口；所有 -D 选项放在 -P 之前，例如：
# cmake -DSAME_BUILD_TESTS=ON -P tools/build.cmake
cmake_minimum_required(VERSION 3.25)
get_filename_component(SAME_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${SAME_SOURCE_DIR}/cmake/BuildRoutes.cmake")
if(NOT DEFINED SAME_CUDA_MODE)
  set(SAME_CUDA_MODE auto)
endif()
if(NOT SAME_CUDA_MODE MATCHES "^(auto|on|off)$")
  message(FATAL_ERROR "SAME_CUDA_MODE must be auto, on or off")
endif()
if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT DEFINED SAME_BUILD_PARALLEL)
  set(SAME_BUILD_PARALLEL 4)
endif()
if(NOT SAME_BUILD_PARALLEL MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "SAME_BUILD_PARALLEL must be positive")
endif()

# Extra options cannot override launcher-owned toolchain/backend decisions.
# 附加选项不得覆盖入口维护的工具链和后端决策。
foreach(_argument IN LISTS SAME_CMAKE_ARGS)
  if(NOT _argument MATCHES "^-D([^:=]+)(:[^=]+)?=")
    message(FATAL_ERROR "SAME_CMAKE_ARGS accepts only -DVAR=VALUE definitions")
  endif()
  set(_key "${CMAKE_MATCH_1}")
  if(_key MATCHES "^(SAME_ENABLE_CUDA|SAME_REQUIRE_CUDA|CMAKE_BUILD_TYPE|CMAKE_.*COMPILER|CMAKE_TOOLCHAIN_FILE|CMAKE_GENERATOR.*)$")
    message(FATAL_ERROR "Pass ${_key} directly before -P, not through SAME_CMAKE_ARGS; use SAME_CUDA_MODE for CUDA policy")
  endif()
endforeach()

# Explicit tools are caller-owned; never silently change their ABI.
# 显式工具链由调用者维护；绝不静默替换编译器或二进制接口。
set(_explicit OFF)
foreach(_key CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ${_key} OR DEFINED ENV{${_key}})
    set(_explicit ON)
  endif()
endforeach()
if(DEFINED ENV{CC} OR DEFINED ENV{CXX})
  set(_explicit ON)
endif()
# Import a captured VsDevCmd environment into this process only.
# 将捕获的 VsDevCmd 环境仅导入当前进程。
function(same_import_vs_environment path)
  file(STRINGS "${path}" _environment ENCODING UTF-16LE)
  foreach(_entry IN LISTS _environment)
    if(_entry MATCHES "^([^=]+)=(.*)$")
      set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
    endif()
  endforeach()
endfunction()

# Capture one x64 environment without probing CUDA; used for CPU fallback.
# 捕获一个不探测 CUDA 的 x64 环境，供 CPU 回退使用。
function(same_capture_vs_environment installation output)
  string(RANDOM LENGTH 12 _nonce)
  set(_directory "$ENV{TEMP}/same-env-${_nonce}")
  file(MAKE_DIRECTORY "${_directory}")
  file(TO_NATIVE_PATH "${installation}/Common7/Tools/VsDevCmd.bat" _setup)
  file(TO_NATIVE_PATH "${_directory}/env.txt" _environment)
  file(WRITE "${_directory}/env.cmd"
    "@echo off\r\ncall \"${_setup}\" -no_logo -arch=x64 -host_arch=x64\r\nif errorlevel 1 exit /b 1\r\nset > \"${_environment}\"\r\n")
  execute_process(COMMAND "$ENV{COMSPEC}" /d /u /c "${_directory}/env.cmd"
    COMMAND_ERROR_IS_FATAL ANY)
  set(${output} "${_directory}/env.txt" PARENT_SCOPE)
endfunction()

# Probe every installed VS instance with the selected nvcc before importing one. A real C++20
# compile/link is authoritative and remains correct when NVIDIA changes its support matrix.
# 导入前用所选 nvcc 探测每个 VS 实例；真实 C++20 编译链接可随 NVIDIA 支持矩阵演进。
if(WIN32 AND NOT _explicit AND NOT SAME_USE_ENVIRONMENT AND NOT DEFINED ENV{VSCMD_VER})
  find_program(_vswhere vswhere
    HINTS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer")
  if(NOT _vswhere)
    message(FATAL_ERROR "vswhere not found; select an x64 compiler environment explicitly")
  endif()
  execute_process(COMMAND "${_vswhere}" -sort -products * -requires
    Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8
    OUTPUT_VARIABLE _vs_json OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  same_visual_studio_installations(_vs_installations "${_vs_json}")
  if(NOT _vs_installations)
    message(FATAL_ERROR "No Visual Studio instance with x64 C++ tools was found")
  endif()

  set(_vs_selected "")
  if(NOT SAME_CUDA_MODE STREQUAL off)
    foreach(_vs IN LISTS _vs_installations)
      string(RANDOM LENGTH 12 _nonce)
      set(_candidate "${SAME_SOURCE_DIR}/build/probes/vs-host-${_nonce}")
      file(MAKE_DIRECTORY "${_candidate}")
      set(_wrapper "set(SAME_CUDA_MODE on)\nset(SAME_TOOLCHAIN_PROBE_ONLY ON)\n")
      string(APPEND _wrapper
        "set(SAME_TOOLCHAIN_PROBE_RESULT [==[${_candidate}/route.txt]==])\n")
      foreach(_key CMAKE_BUILD_TYPE SAME_CMAKE_ARGS CMAKE_CUDA_COMPILER
          CMAKE_CUDA_HOST_COMPILER CMAKE_CUDA_ARCHITECTURES CUDAToolkit_ROOT CMAKE_MAKE_PROGRAM)
        if(DEFINED ${_key})
          string(APPEND _wrapper "set(${_key} [==[${${_key}}]==])\n")
        endif()
      endforeach()
      string(APPEND _wrapper "include([==[${CMAKE_CURRENT_LIST_FILE}]==])\n")
      file(WRITE "${_candidate}/probe.cmake" "${_wrapper}")
      file(TO_NATIVE_PATH "${_vs}/Common7/Tools/VsDevCmd.bat" _setup)
      file(TO_NATIVE_PATH "${CMAKE_COMMAND}" _cmake)
      file(TO_NATIVE_PATH "${_candidate}/probe.cmake" _probe_script)
      file(TO_NATIVE_PATH "${_candidate}/env.txt" _environment)
      file(WRITE "${_candidate}/probe.cmd"
        "@echo off\r\ncall \"${_setup}\" -no_logo -arch=x64 -host_arch=x64\r\nif errorlevel 1 exit /b 1\r\n\"${_cmake}\" -P \"${_probe_script}\"\r\nif errorlevel 1 exit /b 1\r\nset > \"${_environment}\"\r\n")
      execute_process(COMMAND "$ENV{COMSPEC}" /d /u /c "${_candidate}/probe.cmd"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
      file(WRITE "${_candidate}/probe.log" "${_output}\n${_error}")
      file(REMOVE "${_candidate}/probe.cmd" "${_candidate}/probe.cmake")
      if(_result EQUAL 0 AND EXISTS "${_candidate}/env.txt"
          AND EXISTS "${_candidate}/route.txt")
        same_import_vs_environment("${_candidate}/env.txt")
        file(READ "${_candidate}/route.txt" _candidate_route)
        set(_vs_selected "${_vs}")
        message(STATUS
          "same: selected CUDA-compatible x64 environment ${_vs} (${_candidate_route})")
        file(REMOVE "${_candidate}/env.txt")
        break()
      endif()
      message(STATUS
        "same: Visual Studio instance is incompatible with selected CUDA: ${_vs}; inspect ${_candidate}/probe.log")
      file(REMOVE "${_candidate}/env.txt")
    endforeach()
  endif()

  if(NOT _vs_selected)
    if(SAME_CUDA_MODE STREQUAL on)
      message(FATAL_ERROR
        "No installed x64 Visual Studio environment can compile/link with the selected CUDA toolkit")
    endif()
    list(GET _vs_installations 0 _vs_selected)
    same_capture_vs_environment("${_vs_selected}" _cpu_environment)
    same_import_vs_environment("${_cpu_environment}")
    get_filename_component(_cpu_environment_directory "${_cpu_environment}" DIRECTORY)
    file(REMOVE "${_cpu_environment_directory}/env.cmd" "${_cpu_environment}")
    if(NOT SAME_CUDA_MODE STREQUAL off)
      set(_cuda_host_unavailable ON)
      message(STATUS "same: no compatible CUDA host compiler; using CPU route")
    endif()
  endif()
endif()
if(WIN32 AND NOT _explicit AND NOT SAME_USE_ENVIRONMENT AND DEFINED ENV{VSCMD_VER})
  set(CMAKE_C_COMPILER cl)
  set(CMAKE_CXX_COMPILER cl)
endif()

# Resolve caller-relative toolchain paths before changing probe source directories.
# 在更换探测源码目录前统一工具链相对路径，避免同一参数产生不同含义。
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{CMAKE_TOOLCHAIN_FILE})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{CMAKE_TOOLCHAIN_FILE}")
endif()
if(DEFINED CMAKE_TOOLCHAIN_FILE)
  get_filename_component(CMAKE_TOOLCHAIN_FILE "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE)
endif()

# Propagate configuration identically to probe and real project.
# 探测与实际工程必须使用相同配置；附加定义用 SAME_CMAKE_ARGS 列表传递。
set(_definitions)
foreach(_key CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_TOOLCHAIN_FILE
    CMAKE_CUDA_COMPILER CMAKE_CUDA_HOST_COMPILER CMAKE_CUDA_ARCHITECTURES
    CUDAToolkit_ROOT CMAKE_MAKE_PROGRAM)
  if(DEFINED ${_key})
    list(APPEND _definitions "-D${_key}=${${_key}}")
  endif()
endforeach()
list(APPEND _definitions ${SAME_CMAKE_ARGS})
# VS generators ignore explicit CUDA compiler/host selections; do not substitute.
# VS 生成器忽略显式 CUDA 编译器/主机选择；禁止以回退替换这些选择。
set(_explicit_cuda OFF)
if(DEFINED CMAKE_CUDA_COMPILER OR DEFINED CMAKE_CUDA_HOST_COMPILER
    OR DEFINED ENV{CUDACXX} OR DEFINED ENV{CUDAHOSTCXX})
  set(_explicit_cuda ON)
endif()
set(_vs_generator "")
if(WIN32 AND NOT _explicit AND NOT _explicit_cuda AND NOT SAME_USE_ENVIRONMENT AND DEFINED ENV{VisualStudioVersion})
  string(REGEX MATCH "^[0-9]+" _vs_major "$ENV{VisualStudioVersion}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E capabilities OUTPUT_VARIABLE _capabilities
    COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCH "Visual Studio ${_vs_major} [0-9]+" _vs_generator "${_capabilities}")
endif()
if(_cuda_host_unavailable)
  set(_routes cpu)
else()
  same_build_routes(_routes "${SAME_CUDA_MODE}" "${_vs_generator}")
endif()
find_program(_ninja NAMES ninja ninja-build NO_CACHE)
if(DEFINED CMAKE_MAKE_PROGRAM AND EXISTS "${CMAKE_MAKE_PROGRAM}")
  set(_ninja "${CMAKE_MAKE_PROGRAM}")
endif()
set(_selected "")
foreach(_route IN LISTS _routes)
  set(_generator Ninja)
  set(_generator_args)
  if(_route STREQUAL msvc-cuda OR (_route STREQUAL cpu AND NOT _ninja AND _vs_generator))
    set(_generator "${_vs_generator}")
    list(APPEND _generator_args -A x64)
    if(DEFINED ENV{VSINSTALLDIR})
      file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" _vs_instance)
      list(APPEND _generator_args "-DCMAKE_GENERATOR_INSTANCE=${_vs_instance}")
    endif()
  endif()
  if(_route STREQUAL cpu)
    set(_selected "${_route}")
    break()
  endif()
  string(RANDOM LENGTH 12 _nonce)
  set(_probe "${SAME_SOURCE_DIR}/build/probes/${_route}-${_nonce}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SAME_SOURCE_DIR}/cmake/probes/cuda"
    -B "${_probe}" -G "${_generator}" ${_generator_args} ${_definitions}
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
  file(WRITE "${_probe}/probe.log" "${_output}\n${_error}")
  if(_result EQUAL 0)
    set(_selected "${_route}")
    break()
  endif()
  message(STATUS "same: ${_route} unavailable; inspect ${_probe}/probe.log")
endforeach()
if(NOT _selected)
  message(FATAL_ERROR "CUDA required, but SDK and available MSVC CUDA routes failed")
endif()
if(SAME_TOOLCHAIN_PROBE_ONLY)
  if(NOT DEFINED SAME_TOOLCHAIN_PROBE_RESULT)
    message(FATAL_ERROR "SAME_TOOLCHAIN_PROBE_RESULT is required for a toolchain-only probe")
  endif()
  file(WRITE "${SAME_TOOLCHAIN_PROBE_RESULT}" "${_selected}|${_generator}")
  return()
endif()
if(NOT DEFINED SAME_BUILD_DIR)
  string(TOLOWER "${CMAKE_BUILD_TYPE}" _config)
  set(SAME_BUILD_DIR "${SAME_SOURCE_DIR}/build/${CMAKE_HOST_SYSTEM_NAME}-${_selected}-${_config}")
endif()
get_filename_component(SAME_BUILD_DIR "${SAME_BUILD_DIR}" ABSOLUTE)
# Preserve compiler identity across reruns, even though CMake may reset caches.
# 保持重复运行的编译器身份；不能依赖 CMake 自动重置缓存来保证 ABI。
set(_identity "${_selected}|${_generator}|$ENV{CC}|$ENV{CXX}|$ENV{CUDACXX}|$ENV{CUDAHOSTCXX}|$ENV{CMAKE_TOOLCHAIN_FILE}")
foreach(_key CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_TOOLCHAIN_FILE
    CMAKE_CUDA_COMPILER CMAKE_CUDA_HOST_COMPILER)
  string(APPEND _identity "|${_key}=${${_key}}")
endforeach()
find_program(_host_cl cl NO_CACHE)
string(APPEND _identity "|cl=${_host_cl}")
string(SHA256 _identity "${_identity}")
set(_marker "${SAME_BUILD_DIR}/same-toolchain.txt")
if(EXISTS "${SAME_BUILD_DIR}/CMakeCache.txt" AND EXISTS "${_marker}")
  file(READ "${_marker}" _previous)
  if(NOT _previous STREQUAL _identity)
    message(FATAL_ERROR "Toolchain changed; choose a fresh SAME_BUILD_DIR")
  endif()
endif()
set(_cuda ON)
if(_selected STREQUAL cpu)
  set(_cuda OFF)
endif()
message(STATUS "same: route=${_selected}; generator=${_generator}; build=${SAME_BUILD_DIR}")
# CMake rejects generator/compiler cache conflicts; use a fresh directory to switch.
# 切换生成器或编译器必须使用新目录；实际工程失败不可掩盖为 CPU 回退。
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SAME_SOURCE_DIR}" -B "${SAME_BUILD_DIR}"
  -G "${_generator}" ${_generator_args} ${_definitions} "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
  "-DSAME_ENABLE_CUDA=${_cuda}" "-DSAME_REQUIRE_CUDA=${_cuda}"
  COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${_marker}" "${_identity}")
if(NOT SAME_CONFIGURE_ONLY)
  execute_process(COMMAND "${CMAKE_COMMAND}" --build "${SAME_BUILD_DIR}"
    --config "${CMAKE_BUILD_TYPE}" --parallel "${SAME_BUILD_PARALLEL}"
    COMMAND_ERROR_IS_FATAL ANY)
  if(SAME_BUILD_TESTS)
    find_program(_ctest ctest HINTS "${CMAKE_COMMAND}/.." REQUIRED)
    execute_process(COMMAND "${_ctest}" --test-dir "${SAME_BUILD_DIR}"
      -C "${CMAKE_BUILD_TYPE}" --output-on-failure COMMAND_ERROR_IS_FATAL ANY)
  endif()
endif()
