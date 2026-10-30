include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX [=[enum Chain:
  End
  Link:
    names:[String]
    next:Chain
fn make(n:int)->Chain:
  if n==0:Chain.End
  else:Chain.Link {names:["é","","👩"],next:make(n-1)}
fn score(chain:Chain)->int:
  match chain:
    Chain.End:0
    Chain.Link {names,next}:length(names[0])+length(names[1])+length(names[2])+score(next)
]=])
COMPILE(returned "${PREFIX}fn main()->int:\n  score(make(4))\n")
EXPECT(returned 8)
COMPILE(escape "${PREFIX}fn first(chain:Chain)->[String]:\n  match chain:\n    Chain.End:[]\n    Chain.Link {names}:names\nfn main()->[String]:\n  first(make(3))\n")
EXPECT(escape "[\"é\", \"\", \"👩\"]")
COMPILE(shared "${PREFIX}fn main()->int:\n  names=[\"é\",\"\",\"👩\"]\n  tail=Chain.Link {names:names,next:Chain.End}\n  left=Chain.Link {names:names,next:tail}\n  right=Chain.Link {names:names,next:tail}\n  score(left)+score(right)+score(tail)\n")
EXPECT(shared 10)
COMPILE(mutual [=[type Data:
  names:[String]
  next:Link
enum Link:
  End
  More:
    data:Data
fn head(link:Link)->Data:
  match link:
    Link.End:Data {names:[],next:Link.End}
    Link.More {data}:data
fn main()->[String]:
  original=Data {names:["old","é"],next:Link.End}
  root=Data {names:[],next:Link.More {data:original}}
  changed=root with {next:Link.More {data:original with {names:["new","👩"]}}}
  a=head(root.next)
  b=head(changed.next)
  concat(a.names,b.names)
]=])
EXPECT(mutual "[\"old\", \"é\", \"new\", \"👩\"]")
COMPILE(empty "${PREFIX}fn main()->[String]:\n  chain=Chain.Link {names:[],next:Chain.End}\n  match chain:\n    Chain.End:[\"bad\"]\n    Chain.Link {names}:names\n")
EXPECT(empty "[]")
COMPILE(empty-elements "${PREFIX}fn main()->[String]:\n  inner=Chain.Link {names:[\"\",\"\"],next:Chain.End}\n  root=Chain.Link {names:[],next:inner}\n  match root:\n    Chain.End:[]\n    Chain.Link {next}:\n      match next:\n        Chain.End:[]\n        Chain.Link {names}:names\n")
EXPECT(empty-elements "[\"\", \"\"]")
COMPILE(operations "${PREFIX}fn main()->[String]:\n  names=concat([\"é\"+\"!\"],[\"👩\",\"x\"])\n  inner=Chain.Link {names:slice(names,0,2),next:Chain.End}\n  root=Chain.Link {names:[],next:inner}\n  match root:\n    Chain.End:[]\n    Chain.Link {next}:\n      match next:\n        Chain.End:[]\n        Chain.Link {names}:names\n")
EXPECT(operations "[\"é!\", \"👩\"]")
COMPILE(nul "${PREFIX}fn main()->int:\n  inner=Chain.Link {names:[\"é\\0👩\"],next:Chain.End}\n  root=Chain.Link {names:[],next:inner}\n  match root:\n    Chain.End:0\n    Chain.Link {next}:\n      match next:\n        Chain.End:0\n        Chain.Link {names}:\n          if names[0]==\"é\\0👩\":byteLength(names[0])\n          else:0\n")
EXPECT(nul 7)
COMPILE(inactive "${PREFIX}fn main()->int:\n  score(Chain.End)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_FAIL_ALLOC_AT=1 "${OUTPUT_DIR}/inactive"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "0" OR NOT OUT STREQUAL "0\n" OR NOT ERR STREQUAL "")
  message(FATAL_ERROR "Inactive recursive String arrays allocated: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(bounds "${PREFIX}fn main()->String:\n  chain=make(2)\n  match chain:\n    Chain.End:\"\"\n    Chain.Link {names}:names[4]\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/bounds"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
    NOT ERR MATCHES "BoundsFault" OR NOT ERR MATCHES "cleanup: live=0")
  message(FATAL_ERROR "Recursive String array fault cleanup: ${STATUS}: ${OUT}${ERR}")
endif()
foreach(FAIL_AT RANGE 1 50)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/returned"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "8\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive String array result changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive String array allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
