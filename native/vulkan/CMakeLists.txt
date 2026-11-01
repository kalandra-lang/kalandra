# Standalone bring-up avoids installing the compiler's LLVM/MLIR dependencies
# on qualification hosts. Production compiler builds retain their pinned toolchain.
cmake_minimum_required(VERSION 3.28)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(KalandraVulkanBringup LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  include(CTest)
endif()
find_package(Vulkan REQUIRED COMPONENTS glslc)
find_package(Threads REQUIRED)
find_program(KALANDRA_SPIRV_VAL spirv-val REQUIRED)
option(KALANDRA_VULKAN_SANITIZERS "Enable address/undefined sanitizers for bring-up" OFF)
set(_shaders "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ggml")
file(GLOB _shader_dependencies CONFIGURE_DEPENDS "${_shaders}/*.glsl")
foreach(_shader IN ITEMS add mul_mm)
  set(_output "${CMAKE_CURRENT_BINARY_DIR}/${_shader}.spv")
  add_custom_command(OUTPUT "${_output}"
    COMMAND "${Vulkan_GLSLC_EXECUTABLE}" --target-env=vulkan1.1 -O -I "${_shaders}"
      -DDATA_A_F32=1 -DB_TYPE=float -DB_TYPE_SCALAR=float -DD_TYPE=float
      -DLOAD_VEC_A=1 -DLOAD_VEC_B=1 -DFLOAT_TYPE=float
      -DFLOAT_TYPEV2=vec2 -DFLOAT_TYPEV4=vec4 -DACC_TYPE=float -DACC_TYPEV2=vec2
      "${_shaders}/${_shader}.comp" -o "${_output}"
    COMMAND "${KALANDRA_SPIRV_VAL}" --target-env vulkan1.1 "${_output}"
    DEPENDS "${_shaders}/${_shader}.comp" ${_shader_dependencies}
    VERBATIM)
  list(APPEND _outputs "${_output}")
endforeach()
add_custom_target(kalandra-vulkan-shaders DEPENDS ${_outputs})
add_library(kalandra-vulkan STATIC Vulkan.cpp)
target_link_libraries(kalandra-vulkan PUBLIC Vulkan::Vulkan Threads::Threads)
target_include_directories(kalandra-vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(kalandra-vulkan PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
# Temporary bring-up artifact paths. Release packaging/relocation is not implemented.
target_compile_definitions(kalandra-vulkan PRIVATE
  KALANDRA_VULKAN_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(kalandra-vulkan kalandra-vulkan-shaders)
if(KALANDRA_VULKAN_SANITIZERS)
  target_compile_options(kalandra-vulkan PUBLIC -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(kalandra-vulkan PUBLIC -fsanitize=address,undefined)
endif()
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(KALANDRA_SPIRV_DIS spirv-dis REQUIRED)
  add_test(NAME native-vulkan-upstream-audit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit.py"
      "${CMAKE_CURRENT_BINARY_DIR}" --spirv-dis "${KALANDRA_SPIRV_DIS}")
  add_executable(kalandra-vulkan-loader-control tests/LoaderControl.cpp)
  target_link_libraries(kalandra-vulkan-loader-control PRIVATE Vulkan::Vulkan)
  if(KALANDRA_VULKAN_SANITIZERS)
    target_compile_options(kalandra-vulkan-loader-control PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(kalandra-vulkan-loader-control PRIVATE -fsanitize=address,undefined)
  endif()
  add_executable(kalandra-vulkan-tests tests/RuntimeTests.cpp)
  target_compile_options(kalandra-vulkan-tests PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(kalandra-vulkan-tests PRIVATE kalandra-vulkan)
  add_test(NAME native-vulkan-runtime COMMAND kalandra-vulkan-tests)
  set_tests_properties(native-vulkan-runtime PROPERTIES TIMEOUT 120 RESOURCE_LOCK vulkan-gpu)
endif()
