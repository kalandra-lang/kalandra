if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    set(REMOTE_GIT_EXTRA)
    if(VARIANT STREQUAL "debug")
      list(APPEND REMOTE_GIT_EXTRA --cmake "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --config "$<CONFIG>")
      if(TARGET kalandra-metal-source)
        list(APPEND REMOTE_GIT_EXTRA --metal "$<TARGET_FILE:kalandra-metal-source>")
      endif()
    endif()
    add_executable(kalandra-remote-git-tests-${VARIANT} tests/RemoteGitTests.cpp)
    target_compile_options(kalandra-remote-git-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-remote-git-tests-${VARIANT} PRIVATE kalandra-packages-sanitized)
      target_compile_options(kalandra-remote-git-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-remote-git-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-remote-git-tests-${VARIANT} PRIVATE kalandra-packages)
    endif()
    add_test(NAME native-remote-git-${VARIANT} COMMAND "${KALANDRA_PYTHON3}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/RemoteGitFixtures.py"
      --api "$<TARGET_FILE:kalandra-remote-git-tests-${VARIANT}>"
      --compiler "$<TARGET_FILE:kalandra>" --checker "$<TARGET_FILE:kalandra-check>"
      --root "${CMAKE_CURRENT_BINARY_DIR}/remote-git-${VARIANT}" ${REMOTE_GIT_EXTRA})
    set_tests_properties(native-remote-git-${VARIANT} PROPERTIES TIMEOUT 180)
    add_test(NAME native-git-refs-${VARIANT} COMMAND "${KALANDRA_PYTHON3}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/GitRefFixtures.py"
      --api "$<TARGET_FILE:kalandra-remote-git-tests-${VARIANT}>"
      --compiler "$<TARGET_FILE:kalandra>" --checker "$<TARGET_FILE:kalandra-check>"
      --root "${CMAKE_CURRENT_BINARY_DIR}/git-refs-${VARIANT}")
    set_tests_properties(native-git-refs-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
