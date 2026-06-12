cmake_minimum_required(VERSION 3.20)
project(chimera CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- vendored third party ---------------------------------------------------
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1 SQLITE_ENABLE_FTS5)

add_library(blake3 STATIC
  third_party/blake3/blake3.c
  third_party/blake3/blake3_dispatch.c
  third_party/blake3/blake3_portable.c)
target_include_directories(blake3 PUBLIC third_party/blake3)
# Portable-only: APE binaries must not assume host SIMD at compile time.
target_compile_definitions(blake3 PRIVATE
  BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512 BLAKE3_USE_NEON=0)

# ---- chimera core (everything but main, so tests can link it) ---------------
add_library(chimera_core STATIC
  src/hash.cpp
  src/manifest.cpp
  src/walk.cpp
  src/chunk.cpp
  src/ttl.cpp
  src/http.cpp
  src/supervisor.cpp
  src/organs.cpp
  src/extract.cpp
  src/ingest.cpp
  src/search.cpp
)
target_include_directories(chimera_core PUBLIC src third_party)
target_link_libraries(chimera_core PUBLIC sqlite3 blake3)
find_package(Threads REQUIRED)
target_link_libraries(chimera_core PUBLIC Threads::Threads)

add_executable(chimera src/main.cpp)
target_link_libraries(chimera PRIVATE chimera_core)

# ---- tests (native runs only; assert-based, no framework) -------------------
enable_testing()
add_executable(test_spine tests/test_spine.cpp)
target_link_libraries(test_spine PRIVATE chimera_core)
add_test(NAME spine COMMAND test_spine)
