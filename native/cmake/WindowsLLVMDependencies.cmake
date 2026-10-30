# The official LLVM Windows export uses upstream static target names; vcpkg
# installs the same static libraries with its own imported target spelling.
include_guard(GLOBAL)
set(ZLIB_USE_STATIC_LIBS ON)
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)
if(NOT TARGET zstd::libzstd_static AND TARGET zstd::libzstd)
  add_library(zstd::libzstd_static ALIAS zstd::libzstd)
endif()
