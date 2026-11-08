include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(TYPES i8 i16 i32 i64 u8 u16 u32 u64 f32 f64)
foreach(ID RANGE 0 9)
  list(GET TYPES ${ID} TYPE)
  foreach(MODE IN ITEMS identity record closure conditional recursive)
    set(BODY "identity(x)")
    set(PREFIX "fn identity(x:[${TYPE}])->[${TYPE}]:\n  return x\n")
    if(MODE STREQUAL "record")
      set(PREFIX "type Box:\n  values:[${TYPE}]\n")
      set(BODY "box=Box {values:x}\n  box.values")
    elseif(MODE STREQUAL "closure")
      set(PREFIX "")
      set(BODY "f=fn()->[${TYPE}]:x\n  f()")
    elseif(MODE STREQUAL "conditional")
      set(BODY "if length(x)>1:x\n  else:identity(x)")
    elseif(MODE STREQUAL "recursive")
      set(PREFIX "fn descend(x:[${TYPE}],n:i64)->[${TYPE}]:\n  return if n==0:x\n  else:descend(x,n-1)\n")
      set(BODY "descend(x,1000)")
    endif()
    COMPILE("${MODE}-${TYPE}" "${PREFIX}fn roundtrip(x:[${TYPE}])->[${TYPE}]:\n  return ${BODY}\nfn main()->i64:\n  return 0\n" --test-entry roundtrip)
    RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "-DNUMERIC_TYPE=${ID}"
      "${SOURCE_DIR}/tests/ArrayReferenceRuntimeTests.cpp" "${OUTPUT_DIR}/${MODE}-${TYPE}.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/${MODE}-${TYPE}-harness")
    RUN_OK("${OUTPUT_DIR}/${MODE}-${TYPE}-harness")
  endforeach()
  COMPILE("task-${TYPE}" "type Aliases:\n  original:[${TYPE}]\n  result:[${TYPE}]\nfn identity(x:[${TYPE}])->[${TYPE}]:\n  return x\nfn roundtrip(x:[${TYPE}])->Aliases ! TaskError:\n  return try parallel:\n    pending=spawn identity(x)\n    result=try await pending\n    Aliases {original:x,result:result}\nfn main()->i64:\n  return 0\n" --test-entry roundtrip)
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS} "-DNUMERIC_TYPE=${ID}"
    "${SOURCE_DIR}/tests/ArrayReferenceTaskTests.cpp" "${OUTPUT_DIR}/task-${TYPE}.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/task-${TYPE}-harness")
  RUN_OK("${OUTPUT_DIR}/task-${TYPE}-harness")
endforeach()
# A borrowed private return and an unrelated escaping function must not make a
# metadata-only entry import or dereference its host input. The harness uses no
# payload and deliberately impossible offsets/strides, including empty arrays.
COMPILE(borrowed-query "fn query(input:[i64])->i64:\n  f=identity\n  return length(f(input))\nfn identity(input:[i64])->[i64]:\n  return input\nfn capture(input:[i64])->fn()->[i64]:\n  return fn()->[i64]:return input\nfn extra()->i64:\n  a=[1]\n  a=[2,3]\n  return length(a)\nfn main()->i64:\n  return extra()\n" --test-entry query)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  -DNUMERIC_TYPE=3 -DINPUT_RANK=1 -DQUERY_LENGTH=1 -isystem "${LLVM_ROOT}/include"
  "${SOURCE_DIR}/tests/CollectionQueryRuntimeTests.cpp" "${OUTPUT_DIR}/borrowed-query.ll" "${RUNTIME}"
  "-L${LLVM_ROOT}/lib" "-Wl,-rpath,${LLVM_ROOT}/lib" -lLLVM -o "${OUTPUT_DIR}/borrowed-query-harness")
RUN_OK("${OUTPUT_DIR}/borrowed-query-harness")
