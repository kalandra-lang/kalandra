if(BUILD_TESTING)
  foreach(variant IN ITEMS debug sanitized)
    add_executable(kalandra-iterator-contract-tests-${variant} tests/IteratorContractsTests.cpp)
    set(semantics kalandra-semantics)
    if(variant STREQUAL "sanitized")
      set(semantics kalandra-semantics-sanitized)
      target_compile_options(kalandra-iterator-contract-tests-${variant} PRIVATE
        -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-iterator-contract-tests-${variant} PRIVATE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    target_link_libraries(kalandra-iterator-contract-tests-${variant} PRIVATE ${semantics})
    target_compile_options(kalandra-iterator-contract-tests-${variant} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME frontend-iterator-contracts-${variant}
      COMMAND kalandra-iterator-contract-tests-${variant}
        "${CMAKE_CURRENT_BINARY_DIR}/iterator-contracts-${variant}")
    set_tests_properties(frontend-iterator-contracts-${variant} PROPERTIES TIMEOUT 120
      LABELS "feature-frontend;feature-control")
  endforeach()
  add_custom_target(native-iterator-qualification DEPENDS kalandra
    kalandra-iterator-contract-tests-debug kalandra-iterator-contract-tests-sanitized)
endif()
