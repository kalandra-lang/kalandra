include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
RUN_OK("${PYTHON}" "${SOURCE_DIR}/tests/LoopControlTests.py" --compiler "${COMPILER}"
  --output "${OUTPUT_DIR}/source" --variant "${VARIANT}")
COMPILE(cancellation "fn probe(n:i64,faultAt:i64)->i64:\n  total=0\n  for i in range(n):\n    if i==faultAt:\n      zero=0\n      total=total+1/zero\n    else:\n      total=(total+i)%1000003\n    if i<0:\n      break\n    continue\n  return total\nfn main()->i64:\n  callback=probe\n  return callback(0,-1)\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  -DKALANDRA_LOOP_CALLABLE_PROBE
  "${SOURCE_DIR}/tests/LocalRebindingRuntimeTests.cpp" "${OUTPUT_DIR}/cancellation.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/cancellation-harness")
RUN_OK("${OUTPUT_DIR}/cancellation-harness")
COMPILE(task-cancellation "type LoopResult:\n  value:i64\nfn identity(value:i64)->i64:\n  return value\nfn probe(n:i64)->LoopResult ! TaskError:\n  total=0\n  for i in range(n):\n    try parallel:\n      pending=spawn identity(i)\n      observed=try await pending\n      total=total+observed\n      if i==n-1:\n        break\n      continue\n  return LoopResult {value:total}\nfn main()->i64:\n  return 0\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/LocalRebindingTaskTests.cpp" "${OUTPUT_DIR}/task-cancellation.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/task-cancellation-harness")
RUN_OK("${OUTPUT_DIR}/task-cancellation-harness")
COMPILE(ownership "fn probe(n:i64)->i64:\n  saved=[7]\n  total=0\n  for i in range(n):\n    values=generate(i%17,fn(index):return index)\n    if i%2==0:\n      continue\n    total=total+length(values)\n    if i==n-1:\n      break\n  return total+saved[0]\nfn main()->i64:\n  return 0\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/LoopOwnershipRuntimeTests.cpp" "${OUTPUT_DIR}/ownership.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/ownership-harness")
RUN_OK("${OUTPUT_DIR}/ownership-harness")
COMPILE(collection "fn probe(n:i64,faultAt:i64)->i64:\n  values=array(for i in range(n):\n    if i==faultAt:\n      zero=0\n      yield i/zero\n    yield i\n  )\n  return length(values)\nfn main()->i64:\n  return 0\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/CollectRuntimeTests.cpp" "${OUTPUT_DIR}/collection.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/collection-harness")
RUN_OK("${OUTPUT_DIR}/collection-harness")
