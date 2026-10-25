include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(BODY "")
set(PREVIOUS values)
foreach(I RANGE 0 19)
  string(APPEND BODY "  step${I}=-${PREVIOUS}+1\n")
  set(PREVIOUS "step${I}")
endforeach()
set(HELPER "fn long(values:Tensor[float32,?])->Tensor[float32,?]:\n${BODY}  ${PREVIOUS}\n")
COMPILE(chain "${HELPER}fn main()->float32:\n  values:Tensor[float32,3]=tensor([1,2,3])\n  result=long(values)\n  reduceSum(result)\n")
EXPECT(chain 6)
file(READ "${OUTPUT_DIR}/chain.ll" LLVM)
string(REGEX MATCHALL "define [^\n]*@kalandra_fn_elementwise_[0-9]+\\([^\n]*" HELPERS "${LLVM}")
list(LENGTH HELPERS COUNT)
if(NOT COUNT EQUAL 40 OR NOT LLVM MATCHES "kalandra_source_run_v3" OR NOT LLVM MATCHES "noinline")
  message(FATAL_ERROR "Long tensor body lost its bounded checked helpers (${COUNT})")
endif()
foreach(DEFINITION IN LISTS HELPERS)
  string(REGEX MATCH "#[0-9]+" ATTRIBUTE "${DEFINITION}")
  if(ATTRIBUTE STREQUAL "" OR NOT LLVM MATCHES "attributes ${ATTRIBUTE} = [{][^\n]*noinline")
    message(FATAL_ERROR "Outlined numeric helper lost its noinline frame boundary")
  endif()
endforeach()
# Empty inputs retain ordinary shape and allocation behavior across outlined calls.
COMPILE(empty "${HELPER}fn main()->float32:\n  values:Tensor[float32,0]=tensor([])\n  reduceSum(long(values))\n")
EXPECT(empty 0)
message(STATUS "Long float32 tensor bodies preserve unary/binary results and empty inputs through checked outlined helpers")

# A folded function-pointer choice must still use the final checked signature.
COMPILE(pointer [=[fn plus(value:int)->int:
  value+1
fn choose(flag:Bool)->fn(int)->int:
  if flag:plus
  else:plus
fn main()->int:
  selected=choose(true)
  selected(41)
]=])
EXPECT(pointer 42)
