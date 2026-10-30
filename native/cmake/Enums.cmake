if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-enum-tests-${VARIANT} tests/EnumTests.cpp)
    target_compile_options(kalandra-enum-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-enum-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized)
      target_compile_definitions(kalandra-enum-tests-${VARIANT} PRIVATE KALANDRA_ENUM_FRONTEND_ONLY)
      target_compile_options(kalandra-enum-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-enum-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-enum-tests-${VARIANT} PRIVATE kalandra-backend)
    endif()
    add_test(NAME native-enums-frontend-${VARIANT} COMMAND kalandra-enum-tests-${VARIANT})
    set_tests_properties(native-enums-frontend-${VARIANT} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-enums-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/enums-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyEnums.cmake")
    set_tests_properties(native-enums-${VARIANT} PROPERTIES TIMEOUT 600)
    add_test(NAME native-enum-payloads-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>"
        "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/enum-payloads-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyEnumPayloads.cmake")
    set_tests_properties(native-enum-payloads-${VARIANT} PROPERTIES TIMEOUT 600)
  endforeach()
endif()
