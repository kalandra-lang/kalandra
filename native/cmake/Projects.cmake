if(BUILD_TESTING)
  foreach(VARIANT IN ITEMS debug sanitized)
    add_executable(kalandra-library-manifest-tests-${VARIANT} tests/LibraryManifestTests.cpp)
    target_compile_options(kalandra-library-manifest-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-library-manifest-tests-${VARIANT} PRIVATE kalandra-project-sanitized)
      target_compile_options(kalandra-library-manifest-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-library-manifest-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-library-manifest-tests-${VARIANT} PRIVATE kalandra-project)
    endif()
    add_test(NAME native-library-manifests-${VARIANT} COMMAND kalandra-library-manifest-tests-${VARIANT} "${CMAKE_CURRENT_BINARY_DIR}/library-manifests-${VARIANT}")
    set_tests_properties(native-library-manifests-${VARIANT} PROPERTIES TIMEOUT 90)
    add_executable(kalandra-project-tests-${VARIANT} tests/ProjectTests.cpp)
    target_compile_options(kalandra-project-tests-${VARIANT} PRIVATE -Wall -Wextra -Werror)
    if(VARIANT STREQUAL "sanitized")
      target_link_libraries(kalandra-project-tests-${VARIANT} PRIVATE kalandra-semantics-sanitized kalandra-project-sanitized)
      target_compile_definitions(kalandra-project-tests-${VARIANT} PRIVATE KALANDRA_PROJECT_FRONTEND_ONLY)
      target_compile_options(kalandra-project-tests-${VARIANT} PRIVATE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(kalandra-project-tests-${VARIANT} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
      target_link_libraries(kalandra-project-tests-${VARIANT} PRIVATE kalandra-backend kalandra-project)
    endif()
    add_test(NAME native-projects-frontend-${VARIANT} COMMAND kalandra-project-tests-${VARIANT} "${CMAKE_CURRENT_BINARY_DIR}/projects-frontend-${VARIANT}")
    set_tests_properties(native-projects-frontend-${VARIANT} PROPERTIES TIMEOUT 90)
  endforeach()
  if(TARGET kalandra-metal-source)
    add_test(NAME native-library-manifests-metal COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra-metal-source>"
      "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/library-manifests-metal"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyLibraryMetal.cmake")
    set_tests_properties(native-library-manifests-metal PROPERTIES TIMEOUT 90)
  endif()
  foreach(VARIANT IN ITEMS debug optimized sanitized thread-sanitized)
    add_test(NAME native-library-cli-${VARIANT} COMMAND "${CMAKE_COMMAND}"
      "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>"
      "-DVARIANT=${VARIANT}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/library-cli-${VARIANT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyLibraryCLI.cmake")
    set_tests_properties(native-library-cli-${VARIANT} PROPERTIES TIMEOUT 90)
    if(VARIANT STREQUAL "sanitized")
      set(PROJECT_RUNTIME kalandra-native-runtime-sanitized)
    elseif(VARIANT STREQUAL "thread-sanitized")
      set(PROJECT_RUNTIME kalandra-native-runtime-thread-sanitized)
    else()
      set(PROJECT_RUNTIME kalandra-native-runtime)
    endif()
    add_test(NAME native-projects-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>" "-DVARIANT=${VARIANT}"
        "-DCLANGXX=${CMAKE_CXX_COMPILER}" "-DRUNTIME=$<TARGET_FILE:${PROJECT_RUNTIME}>"
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/projects-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyProjects.cmake")
    set_tests_properties(native-projects-${VARIANT} PROPERTIES TIMEOUT 300)
    add_test(NAME native-project-cli-${VARIANT}
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:kalandra>" "-DCHECKER=$<TARGET_FILE:kalandra-check>" "-DVARIANT=${VARIANT}"
        "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/project-cli-${VARIANT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyProjectCLI.cmake")
    set_tests_properties(native-project-cli-${VARIANT} PROPERTIES TIMEOUT 90)
  endforeach()
endif()
