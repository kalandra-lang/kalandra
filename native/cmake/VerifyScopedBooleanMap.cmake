include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
COMPILE(repeated "fn main()->i64 ! TaskError:\n  a=[7]\n  i=0\n  total=0\n  while i<100:\n    a=[i+1]\n    flags=try parallelMap(generate(i%17,fn(j):return j),fn(value):return value%2==0)\n    j=0\n    while j<length(flags):\n      if flags[j]:total=total+1\n      j=j+1\n    i=i+1\n  return total+a[0]\n")
EXPECT(repeated 516)
COMPILE(large "fn main()->i64 ! TaskError:\n  a=[1]\n  a=[2]\n  flags=try parallelMap(generate(8193,fn(j):return j),fn(value):return value%2==0)\n  i=0\n  correct=0\n  while i<length(flags):\n    if flags[i]==(i%2==0):correct=correct+1\n    i=i+1\n  return correct+a[0]\n")
EXPECT(large 8195)
COMPILE(errors "error Stop:\n  Bad:\n    reason:String\nfn predicate(value:i64)->bool ! Stop:\n  return if value==2:fail Stop.Bad {reason:\"valid\"+\"!\"}\n  else:value%2==0\nfn main()->i64:\n  a=[1]\n  a=[2]\n  flags=parallelMap([0,1,2],predicate) catch fn(error):\n    return match error:\n      Stop.Bad {reason}:[length(reason)==6,false]\n      TaskError.Fault:[false]\n      TaskError.Cancelled:[false]\n      TaskError.DeadlineExceeded:[false]\n      TaskError.AdmissionRejected:[false]\n  return if flags[0] and not flags[1]:42+a[0]\n  else:0\n")
EXPECT(errors 44)
COMPILE(probe "type LoopResult:\n  value:i64\nfn read(a:[i64])->i64:\n  return a[0]\nfn probe(n:i64,faultAt:i64)->LoopResult ! TaskError:\n  a=[7,8]\n  saved=a\n  i=0\n  total=0\n  while i<n:\n    value=try parallel:\n      old=spawn read(a)\n      a=if i%2==0:[1,2,3]\n      else:[4]\n      try await old\n    flags=try parallelMap([0,1,2,3,4,5,6,7,8],fn(value):return value%2==0)\n    total=total+value\n    j=0\n    while j<length(flags):\n      if flags[j]:total=total+1\n      j=j+1\n    if i==faultAt:\n      zero=0\n      total=total+1/zero\n    i=i+1\n  return LoopResult {value:saved[0]+total+a[0]+length(a)}\nfn main()->i64:\n  return 0\n" --test-entry probe)
RUN_OK("${CLANGXX}" -std=c++23 -Wall -Wextra -Werror -Wno-override-module ${FLAGS}
  -DEXPECTED_PER_PAIR=15 -DTASK_IN_FLIGHT=16 "${SOURCE_DIR}/tests/ArrayRebindingTaskTests.cpp"
  "${OUTPUT_DIR}/probe.ll" "${RUNTIME}" -o "${OUTPUT_DIR}/probe-harness")
RUN_OK("${OUTPUT_DIR}/probe-harness")
# Owned cells are now built through initialized-prefix ownership, not zeroing.
COMPILE(owned "fn main()->i64 ! TaskError:\n  a=[1]\n  a=[2]\n  values=try parallelMap([1,2],fn(value):return \"text\")\n  return length(values)+a[0]\n")
EXPECT(owned 4)
