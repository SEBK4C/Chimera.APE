# Cosmopolitan toolchain for the chimera orchestrator.
# Adapted from vendor/qlever-cosmopolitan/toolchains/cosmocc.cmake.
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=toolchains/cosmocc.cmake ..

set(COSMOCC_ROOT "$ENV{COSMOCC}")
if(NOT COSMOCC_ROOT)
  set(COSMOCC_ROOT "$ENV{HOME}/cosmocc")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   "${COSMOCC_ROOT}/bin/cosmocc"   CACHE FILEPATH "cc")
set(CMAKE_CXX_COMPILER "${COSMOCC_ROOT}/bin/cosmoc++"  CACHE FILEPATH "c++")
set(CMAKE_AR           "${COSMOCC_ROOT}/bin/cosmoar"   CACHE FILEPATH "ar")
set(CMAKE_RANLIB       "${COSMOCC_ROOT}/bin/cosmoranlib" CACHE FILEPATH "ranlib")

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# cosmocc emits fat x86_64+aarch64 binaries; nothing target-specific belongs
# in compile flags here.
