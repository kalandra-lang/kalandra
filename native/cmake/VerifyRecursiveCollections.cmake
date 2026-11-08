include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
foreach(TYPE IN ITEMS i8 u8 i16 u16 i32 u32 i64 u64 f32 f64)
  set(DECL "enum Chain:\n  End\n  Link:\n    values:[${TYPE}]\n    fixed:Tensor[${TYPE},1,2]\n    dynamic:Tensor[${TYPE},?,?]\n    scalar:Tensor[${TYPE}]\n    zero:Tensor[${TYPE},2,0,3]\n    next:Chain\nfn make()->Chain:\n  return Chain.Link {values:[1,2],fixed:tensor([[3,4]]),dynamic:tensor([[5,6]]),scalar:tensor(7),zero:tensor([],[2,0,3]),next:Chain.End}\nfn arrayScore(chain:Chain)->${TYPE}:\n  return match chain:\n    Chain.End:0\n    Chain.Link {values}:values[1]\nfn fixedScore(chain:Chain)->${TYPE}:\n  return match chain:\n    Chain.End:0\n    Chain.Link {fixed}:fixed[0,1]\nfn dynamicScore(chain:Chain)->${TYPE}:\n  return match chain:\n    Chain.End:0\n    Chain.Link {dynamic}:dynamic[0,1]\nfn scalarScore(chain:Chain)->${TYPE}:\n  return match chain:\n    Chain.End:0\n    Chain.Link {scalar}:reduceSum(scalar)\nfn emptyScore(chain:Chain)->${TYPE}:\n  return match chain:\n    Chain.End:0\n    Chain.Link {zero}:reduceSum(zero)\nfn sum(chain:Chain)->${TYPE}:\n  return match chain:\n    Chain.End:0\n    Chain.Link {next}:arrayScore(chain)+fixedScore(chain)+dynamicScore(chain)+scalarScore(chain)+emptyScore(chain)+sum(next)\n")
  COMPILE("collections-${TYPE}" "${DECL}fn main()->i64:\n  tail=make()\n  root=Chain.Link {values:[1,2],fixed:tensor([[3,4]]),dynamic:tensor([[5,6]]),scalar:tensor(7),zero:tensor([],[2,0,3]),next:tail}\n  return i64(sum(root)+sum(tail))\n")
  EXPECT("collections-${TYPE}" 57)
endforeach()
set(PREFIX [=[enum Chain:
  End
  Link:
    values:[i64]
    flags:[bool]
    text:String
    next:Chain
fn make(n:i64)->Chain:
  if n==0:Chain.End
  else:Chain.Link {values:[n,n+1],flags:[true,false],text:"é",next:make(n-1)}
fn sum(chain:Chain)->i64:
  match chain:
    Chain.End:0
    Chain.Link {values,flags,text,next}:
      if flags[0]:values[0]+values[1]+length(text)+sum(next)
      else:0
]=])
COMPILE(returned "${PREFIX}fn main()->i64:\n  return sum(make(4))\n")
EXPECT(returned 28)
COMPILE(escape "${PREFIX}fn first(chain:Chain)->[i64]:\n  return match chain:\n    Chain.End:[]\n    Chain.Link {values}:values\nfn main()->[i64]:\n  return first(make(4))\n")
EXPECT(escape "[4, 5]")
COMPILE(bool-escape "${PREFIX}fn flags(chain:Chain)->[bool]:\n  return match chain:\n    Chain.End:[]\n    Chain.Link {flags}:flags\nfn main()->[bool]:\n  return flags(make(2))\n")
EXPECT(bool-escape "[true, false]")
COMPILE(mutual [=[type Data:
  values:[i64]
  flags:[bool]
  next:Link
enum Link:
  End
  More:
    data:Data
fn head(link:Link)->Data:
  match link:
    Link.End:Data {values:[],flags:[],next:Link.End}
    Link.More {data}:data
fn main()->[i64]:
  original=Data {values:[10,20],flags:[true],next:Link.End}
  root=Data {values:[],flags:[],next:Link.More {data:original}}
  changed=root with {next:Link.More {data:original with {values:[30,40]}}}
  a=head(root.next)
  b=head(changed.next)
  concat(a.values,b.values)
]=])
EXPECT(mutual "[10, 20, 30, 40]")
COMPILE(inactive "${PREFIX}fn main()->i64:\n  return sum(Chain.End)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_FAIL_ALLOC_AT=1 "${OUTPUT_DIR}/inactive"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "0" OR NOT OUT STREQUAL "0\n" OR NOT ERR STREQUAL "")
  message(FATAL_ERROR "Inactive recursive collections allocated: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(shape [=[enum Chain:
  End
  Link:
    value:Tensor[i64,2]
    next:Chain
fn make(value:Tensor[i64,?])->Chain:
  return Chain.Link {value:value,next:Chain.End}
fn dynamic(n:i64)->i64:
  chain=make(tensor([1,2,3],[n]))
  return 0
fn main()->i64:
  return dynamic(3)
]=])
COMPILE(bounds "${PREFIX}fn main()->i64:\n  chain=make(2)\n  return match chain:\n    Chain.End:0\n    Chain.Link {values}:values[3]\n")
foreach(NAME IN ITEMS shape bounds)
  if(NAME STREQUAL "shape")
    set(EXPECTED_STATUS 87)
    set(EXPECTED_FAULT ShapeFault)
  else()
    set(EXPECTED_STATUS 86)
    set(EXPECTED_FAULT BoundsFault)
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/${NAME}"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(NOT "${STATUS}" STREQUAL "${EXPECTED_STATUS}" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "${EXPECTED_FAULT}" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive collection ${NAME} fault cleanup: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
foreach(FAIL_AT RANGE 1 45)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/returned"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "28\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive collection success changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive collection allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
