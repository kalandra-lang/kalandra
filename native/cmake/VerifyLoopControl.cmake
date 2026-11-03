include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
RUN_OK("${PYTHON}" "${SOURCE_DIR}/tests/LoopControlTests.py" --compiler "${COMPILER}"
  --output "${OUTPUT_DIR}/source" --variant "${VARIANT}")
COMPILE(cancellation "fn probe(n:int,faultAt:int)->int:\n  total=0\n  for i in range(n):\n    if i==faultAt:\n      zero=0\n      total=total+1/zero\n    else:\n      total=(total+i)%1000003\n    if i<0:\n      break\n    continue\n  total\nfn main()->int:\n  callback=probe\n  callback(0,-1)\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  -DKALANDRA_LOOP_CALLABLE_PROBE
  "${SOURCE_DIR}/tests/LocalRebindingRuntimeTests.cpp" "${OUTPUT_DIR}/cancellation.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/cancellation-harness")
RUN_OK("${OUTPUT_DIR}/cancellation-harness")
COMPILE(task-cancellation "type LoopResult:\n  value:int\nfn identity(value:int)->int:\n  value\nfn probe(n:int)->LoopResult ! TaskError:\n  total=0\n  for i in range(n):\n    try parallel:\n      pending=spawn identity(i)\n      observed=try await pending\n      total=total+observed\n      if i==n-1:\n        break\n      continue\n  LoopResult {value:total}\nfn main()->int:\n  0\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/LocalRebindingTaskTests.cpp" "${OUTPUT_DIR}/task-cancellation.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/task-cancellation-harness")
RUN_OK("${OUTPUT_DIR}/task-cancellation-harness")
COMPILE(ownership "fn probe(n:int)->int:\n  saved=[7]\n  total=0\n  for i in range(n):\n    values=generate(i%17,fn(index):index)\n    if i%2==0:\n      continue\n    total=total+length(values)\n    if i==n-1:\n      break\n  total+saved[0]\nfn main()->int:\n  0\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  "${SOURCE_DIR}/tests/LoopOwnershipRuntimeTests.cpp" "${OUTPUT_DIR}/ownership.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/ownership-harness")
RUN_OK("${OUTPUT_DIR}/ownership-harness")
