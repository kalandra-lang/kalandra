# The official archive exports a static Zstd target name. Tumbleweed's devel
# package provides the compatible shared C library instead. This explicit
# development-only adapter uses that installed library; it does not claim a
# statically linked/reproducible release package or alter the archive/system.
if(KALANDRA_DEVELOPMENT_LLVM_VERSION)
  find_package(zstd CONFIG REQUIRED)
  if(NOT TARGET zstd::libzstd_static AND TARGET zstd::libzstd_shared)
    add_library(zstd::libzstd_static INTERFACE IMPORTED GLOBAL)
    set_target_properties(zstd::libzstd_static PROPERTIES
      INTERFACE_LINK_LIBRARIES zstd::libzstd_shared)
    message(STATUS "Development LLVM links installed shared Zstd; installation remains disabled")
  endif()
endif()
