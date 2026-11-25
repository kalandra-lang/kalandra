# The release lock stays authoritative. This opt-in profile qualifies the user's
# installed host Clang against matching private LLVM/MLIR 23.1.x development files.
set(KALANDRA_DEVELOPMENT_LLVM_VERSION "" CACHE STRING "Non-release host LLVM 23.1.x toolchain qualification version")
if(KALANDRA_DEVELOPMENT_LLVM_VERSION)
  set(_valid_dev_llvm FALSE)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND KALANDRA_DEVELOPMENT_LLVM_VERSION STREQUAL "23.1.4")
    set(_valid_dev_llvm TRUE)
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND KALANDRA_DEVELOPMENT_LLVM_VERSION MATCHES "^23\\.1\\.[0-9]+$")
    set(_valid_dev_llvm TRUE)
  endif()
  if(NOT _valid_dev_llvm)
    message(FATAL_ERROR "Unsupported development LLVM/Clang/MLIR override: ${CMAKE_HOST_SYSTEM_NAME} ${KALANDRA_DEVELOPMENT_LLVM_VERSION}")
  endif()
  set(KALANDRA_LLVM_VERSION "${KALANDRA_DEVELOPMENT_LLVM_VERSION}")
  # An experimental patch-level build must never become a release package just
  # because someone runs cmake --install after local qualification.
  set(CMAKE_SKIP_INSTALL_RULES ON)
  message(WARNING "Development-only LLVM ${KALANDRA_LLVM_VERSION}: install rules disabled; release lock unchanged")
endif()
