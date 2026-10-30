if(BUILD_TESTING)
  add_test(NAME native-starter-project COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ScaffoldTests.py" "$<TARGET_FILE:kalandra>"
    "$<TARGET_FILE:kalandra-check>" "$<TARGET_FILE:kalandra-fmt>")
  set_tests_properties(native-starter-project PROPERTIES TIMEOUT 120)
endif()
