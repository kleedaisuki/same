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
if(WIN32 AND NOT _explicit AND NOT SAME_USE_ENVIRONMENT)
  if(NOT DEFINED ENV{VSCMD_VER})
    find_program(_vswhere vswhere HINTS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer")
    if(_vswhere)
      execute_process(COMMAND "${_vswhere}" -latest -products * -requires
        Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        OUTPUT_VARIABLE _vs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
      if(_vs)
        # Import only this process' environment; no global PATH/IDE mutations.
        # 仅导入当前进程环境，不修改全局 PATH 或 IDE 设置。
        string(RANDOM LENGTH 12 _nonce)
        set(_env_dir "$ENV{TEMP}/same-env-${_nonce}")
        file(MAKE_DIRECTORY "${_env_dir}")
        set(ENV{SAME_VS_SETUP} "${_vs}/Common7/Tools/VsDevCmd.bat")
        set(ENV{SAME_ENV_OUTPUT} "${_env_dir}/env.txt")
        file(WRITE "${_env_dir}/env.cmd"
          "@echo off\r\ncall \"%SAME_VS_SETUP%\" -no_logo -arch=x64 -host_arch=x64\r\nif errorlevel 1 exit /b 1\r\nset > \"%SAME_ENV_OUTPUT%\"\r\n")
        execute_process(COMMAND "$ENV{COMSPEC}" /d /u /c "${_env_dir}/env.cmd"
          COMMAND_ERROR_IS_FATAL ANY)
        file(STRINGS "${_env_dir}/env.txt" _environment ENCODING UTF-16LE)
        foreach(_entry IN LISTS _environment)
          if(_entry MATCHES "^([^=]+)=(.*)$")
            set(ENV{${CMAKE_MATCH_1}} "${CMAKE_MATCH_2}")
          endif()
        endforeach()
        file(REMOVE "${_env_dir}/env.cmd" "${_env_dir}/env.txt")
      endif()
    endif()
  endif()
  if(DEFINED ENV{VSCMD_VER})
    set(CMAKE_C_COMPILER cl)
    set(CMAKE_CXX_COMPILER cl)
  endif()
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
same_build_routes(_routes "${SAME_CUDA_MODE}" "${_vs_generator}")
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
