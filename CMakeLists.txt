cmake_minimum_required(VERSION 3.25)
project(same VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SAME_ENABLE_CUDA "Build CUDA backend when a supported compiler is available" ON)
option(SAME_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
include(CTest)
include(FetchContent)
find_package(Threads REQUIRED)

# Pinned and verified dependencies / 固定版本并校验依赖。
FetchContent_Declare(blake3
  URL https://codeload.github.com/BLAKE3-team/BLAKE3/tar.gz/refs/tags/1.8.2
  URL_HASH SHA256=6b51aefe515969785da02e87befafc7fdc7a065cd3458cf1141f29267749e81f
  SOURCE_SUBDIR c DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(tomlplusplus
  URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
  URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(sqlite_source
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(blake3 tomlplusplus sqlite_source)
add_library(sqlite3 STATIC "${sqlite_source_SOURCE_DIR}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "${sqlite_source_SOURCE_DIR}")
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0
  SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_LOAD_EXTENSION)
target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

set(SAME_CUDA OFF)
if(SAME_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(SAME_CUDA ON)
  else()
    message(STATUS "CUDA compiler unavailable: building CPU fallback")
  endif()
endif()

add_library(same_core
  src/config.cpp src/files.cpp src/store.cpp src/compute_cpu.cpp
  src/resources.cpp src/application.cpp src/run_lock.cpp)
target_include_directories(same_core PUBLIC include)
target_link_libraries(same_core PUBLIC sqlite3 BLAKE3::blake3 tomlplusplus::tomlplusplus Threads::Threads)
if(SAME_CUDA)
  target_sources(same_core PRIVATE src/compute_cuda.cu)
  set_target_properties(same_core PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
else()
  target_sources(same_core PRIVATE src/compute_stub.cpp)
endif()
if(MSVC)
  target_compile_options(same_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8>)
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
  endforeach()
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME integration COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py" "$<TARGET_FILE:same>")
  endif()
endif()
