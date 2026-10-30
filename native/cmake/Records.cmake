if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-record-tests-${VARIANT} tests/RecordTests.cpp)
    target_compile_options(kalandra-record-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-record-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-record-tests-${VARIANT} PRIVATE KALANDRA_RECORD_FRONTEND_ONLY)
      target_compile_options(kalandra-record-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-record-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-record-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-records-frontend-${VARIANT} COMMAND kalandra-record-tests-${VARIANT}
      "${CMAKE_CURRENT_BINARY_DIR}/records-frontend-${VARIANT}")
    set_tests_properties(native-records-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-records-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/records-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRecords.cmake")
    set_tests_properties(native-records-${VARIANT} PROPERTIES TIMEOUT 900)
  endforeach()
endif()
