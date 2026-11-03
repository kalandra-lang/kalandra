include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
foreach(TYPE IN ITEMS int8 float64)
  set(NAME "fresh-${TYPE}")
  COMPILE("${NAME}" "type LoopResult:\n  value:int\nfn read(a:[${TYPE}])->int:\n  int(a[0])\nfn probe(n:int,faultAt:int)->LoopResult ! TaskError:\n  a=[${TYPE}(7),${TYPE}(8)]\n  saved=a\n  i=0\n  total=0\n  while i<n:\n    value=try parallel:\n      old=spawn read(a)\n      a=sorted(a)\n      a[0]=${TYPE}(9)\n      try await old\n    total=total+value\n    if i==faultAt:\n      zero=0\n      total=total+1/zero\n    i=i+1\n  LoopResult {value:int(saved[0])+total+int(a[0])+length(a)}\nfn main()->int:\n  0\n" --test-entry probe)
  RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
    -DEXPECTED_BIAS=16 -DEXPECTED_PER_PAIR=18
    "${SOURCE_DIR}/tests/ArrayRebindingTaskTests.cpp" "${OUTPUT_DIR}/${NAME}.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/${NAME}-harness")
  RUN_OK("${OUTPUT_DIR}/${NAME}-harness")
endforeach()
COMPILE(nested-rhs "fn main()->int ! TaskError:\n  a=[1]\n  i=0\n  total=0\n  while i<3:\n    a=copy(a)\n    saved=a\n    a[0]=if true:\n      j=0\n      while j<2:\n        a=copy(a)\n        j=j+1\n      4\n    else:0\n    total=total+saved[0]\n    i=i+1\n  total+a[0]\n")
EXPECT(nested-rhs 13)
