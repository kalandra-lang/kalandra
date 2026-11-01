# The release lock stays authoritative. This opt-in profile qualifies the user's
# installed Linux Clang against matching private LLVM/MLIR development files.
set(KALANDRA_DEVELOPMENT_LLVM_VERSION "" CACHE STRING "Non-release Linux Vulkan toolchain qualification version")
if(KALANDRA_DEVELOPMENT_LLVM_VERSION)
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
     NOT KALANDRA_DEVELOPMENT_LLVM_VERSION STREQUAL "23.1.4")
    message(FATAL_ERROR "The only additional development toolchain under qualification is Linux LLVM/Clang/MLIR 23.1.4")
  endif()
  set(KALANDRA_LLVM_VERSION "${KALANDRA_DEVELOPMENT_LLVM_VERSION}")
  # An experimental patch-level build must never become a release package just
  # because someone runs cmake --install after local qualification.
  set(CMAKE_SKIP_INSTALL_RULES ON)
  message(WARNING "Development-only LLVM ${KALANDRA_LLVM_VERSION}: install rules disabled; release lock unchanged")
endif()
