# Pinned dependencies shared by the native targets. / 原生目标共享的固定依赖。
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
