if(BUILD_TESTING)
  foreach(LOCK_VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-package-lock-tests-${LOCK_VARIANT} tests/PackageLockTests.cpp)
    target_compile_options(kalandra-package-lock-tests-${LOCK_VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(LOCK_VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-package-lock-tests-${LOCK_VARIANT} PRIVATE kalandra-packages-sanitized)
      target_compile_options(kalandra-package-lock-tests-${LOCK_VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-package-lock-tests-${LOCK_VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-package-lock-tests-${LOCK_VARIANT} PRIVATE kalandra-packages)
    endif()
    add_test(NAME native-package-lock-${LOCK_VARIANT} COMMAND "${KALANDRA_PYTHON3}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackageLockFixtures.py"
      --api "$<TARGET_FILE:kalandra-package-lock-tests-${LOCK_VARIANT}>"
      --compiler "$<TARGET_FILE:kalandra>" --checker "$<TARGET_FILE:kalandra-check>"
      --root "${CMAKE_CURRENT_BINARY_DIR}/package-lock-${LOCK_VARIANT}")
    set_tests_properties(native-package-lock-${LOCK_VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
  add_executable(kalandra-package-install-tests-sanitized tests/PackageInstallTests.cpp)
  target_link_libraries(kalandra-package-install-tests-sanitized PRIVATE kalandra-packages-sanitized)
  target_compile_options(kalandra-package-install-tests-sanitized PRIVATE -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(kalandra-package-install-tests-sanitized PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    set(INSTALL_EXTRA)
    if(VARIANT STREQUAL "debug")
      if(TARGET kalandra-metal-source)
        list(APPEND INSTALL_EXTRA --metal "$<TARGET_FILE:kalandra-metal-source>")
      endif()
    endif()
    add_test(NAME native-package-install-${VARIANT} COMMAND "${KALANDRA_PYTHON3}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackageInstallFixtures.py"
      --compiler "$<TARGET_FILE:kalandra>" --checker "$<TARGET_FILE:kalandra-check>"
      --variant "${VARIANT}" --root "${CMAKE_CURRENT_BINARY_DIR}/package-install-${VARIANT}" ${INSTALL_EXTRA})
    set_tests_properties(native-package-install-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
  add_test(NAME native-package-install-frontend-sanitized COMMAND "${KALANDRA_PYTHON3}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackageInstallFixtures.py"
    --compiler "$<TARGET_FILE:kalandra>" --checker "$<TARGET_FILE:kalandra-check-sanitized>"
    --api "$<TARGET_FILE:kalandra-package-install-tests-sanitized>"
    --root "${CMAKE_CURRENT_BINARY_DIR}/package-install-frontend-sanitized")
  set_tests_properties(native-package-install-frontend-sanitized PROPERTIES TIMEOUT 240)
endif()

if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-package-update-tests-${VARIANT} tests/PackageUpdateTests.cpp)
    target_compile_options(kalandra-package-update-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-package-update-tests-${VARIANT} PRIVATE kalandra-packages-sanitized)
      target_compile_options(kalandra-package-update-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-package-update-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-package-update-tests-${VARIANT} PRIVATE kalandra-packages)
    endif()
    add_test(NAME native-package-update-${VARIANT} COMMAND "${KALANDRA_PYTHON3}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackageUpdateFixtures.py"
      --api "$<TARGET_FILE:kalandra-package-update-tests-${VARIANT}>"
      --compiler "$<TARGET_FILE:kalandra>" --checker "$<TARGET_FILE:kalandra-check>"
      --root "${CMAKE_CURRENT_BINARY_DIR}/package-update-${VARIANT}")
    set_tests_properties(native-package-update-${VARIANT} PROPERTIES TIMEOUT 180)
  endforeach()
endif()
