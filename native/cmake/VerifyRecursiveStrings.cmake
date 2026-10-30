include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX [=[enum Texts:
  Empty
  More:
    text:String
    next:Texts
fn join(items:Texts)->String:
  match items:
    Texts.Empty:""
    Texts.More {text,next}:text+join(next)
]=])
COMPILE(returned "${PREFIX}fn make(n:int)->Texts:\n  if n==0:Texts.Empty\n  else:Texts.More {text:\"é👩\",next:make(n-1)}\nfn main()->String:\n  join(make(3))\n")
EXPECT(returned "é👩é👩é👩")
COMPILE(shared "${PREFIX}fn main()->String:\n  tail=Texts.More {text:\"👩\",next:Texts.Empty}\n  left=Texts.More {text:\"a\",next:tail}\n  right=Texts.More {text:\"é\",next:tail}\n  join(left)+join(right)+join(tail)\n")
EXPECT(shared "a👩é👩👩")
COMPILE(empty "${PREFIX}fn main()->String:\n  join(Texts.More {text:\"\",next:Texts.More {text:\"\",next:Texts.Empty}})\n")
EXPECT(empty "")
COMPILE(mutual [=[type Label:
  text:String
  next:Link
enum Link:
  End
  More:
    label:Label
fn labelText(link:Link)->String:
  match link:
    Link.End:""
    Link.More {label}:label.text
fn main()->String:
  original=Label {text:"old",next:Link.End}
  root=Label {text:"root",next:Link.More {label:original}}
  changed=root with {text:"new",next:Link.More {label:original with {text:"é"}}}
  labelText(root.next)+labelText(changed.next)+root.text+changed.text
]=])
EXPECT(mutual "oldérootnew")
COMPILE(operations "${PREFIX}fn tail(items:Texts)->Texts:\n  match items:\n    Texts.Empty:Texts.Empty\n    Texts.More {next}:next\nfn main()->String:\n  text=\"aé👩\"\n  items=Texts.More {text:\"ignore\",next:Texts.More {text:slice(text,1,3)+text[0],next:Texts.Empty}}\n  join(tail(items))\n")
EXPECT(operations "é👩a")
COMPILE(embedded-nul "${PREFIX}fn main()->int64:\n  text=join(Texts.More {text:\"é\\0👩\",next:Texts.Empty})\n  if text==\"é\\0👩\":byteLength(text)+length(text)\n  else:0\n")
EXPECT(embedded-nul 10)
COMPILE(fault "${PREFIX}fn main()->String:\n  items=Texts.More {text:\"é\",next:Texts.Empty}\n  join(items)[2]\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/fault"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
    NOT ERR MATCHES "BoundsFault" OR NOT ERR MATCHES "cleanup: live=0")
  message(FATAL_ERROR "Recursive String fault: ${STATUS}: ${OUT}${ERR}")
endif()
foreach(FAIL_AT RANGE 1 20)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/returned"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "é👩é👩é👩\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive String allocation success changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive String allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
