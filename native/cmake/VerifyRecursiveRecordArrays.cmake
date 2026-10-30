include("${CMAKE_CURRENT_LIST_DIR}/Float64Driver.cmake")
set(PREFIX [=[type Entry:
  name:String
  values:[int]
  flags:[Bool]
  names:[String]
enum Chain:
  End
  Link:
    entries:[Entry]
    next:Chain
fn make()->Chain:
  entry=Entry {name:"é",values:[20,22],flags:[true],names:["👩",""]}
  Chain.Link {entries:[entry,entry],next:Chain.End}
fn score(chain:Chain)->int:
  match chain:
    Chain.End:0
    Chain.Link {entries,next}:
      if length(entries)==0:score(next)
      else:
        entry=entries[1]
        if entry.flags[0]:entry.values[0]+entry.values[1]+length(entry.name)+length(entry.names[0])+score(next)
        else:0
]=])
COMPILE(returned "${PREFIX}fn main()->int:\n  tail=make()\n  root=Chain.Link {entries:[],next:tail}\n  score(root)+score(tail)\n")
EXPECT(returned 88)
COMPILE(escape "${PREFIX}fn entries(chain:Chain)->[Entry]:\n  match chain:\n    Chain.End:[]\n    Chain.Link {entries}:entries\nfn main()->String:\n  values=entries(make())\n  values[0].name+values[1].names[0]\n")
EXPECT(escape "é👩")
COMPILE(mixed [=[enum Item:
  Empty
  Text:
    value:String
  Values:
    data:[int]
enum Chain:
  End
  Link:
    items:[Item]
    next:Chain
fn itemScore(item:Item)->int:
  match item:
    Item.Empty:0
    Item.Text {value}:length(value)
    Item.Values {data}:data[0]
fn score(chain:Chain)->int:
  match chain:
    Chain.End:0
    Chain.Link {items,next}:itemScore(items[0])+itemScore(items[1])+itemScore(items[2])+score(next)
fn main()->int:
  inner=Chain.Link {items:[Item.Empty,Item.Text {value:"é"},Item.Values {data:[41]}],next:Chain.End}
  root=Chain.Link {items:[],next:inner}
  match root:
    Chain.End:0
    Chain.Link {next}:score(next)
]=])
EXPECT(mixed 42)
set(TREE [=[type Tree:
  name:String
  children:[Tree]
fn leaves()->[Tree]:
  [Tree {name:"é",children:[]},Tree {name:"👩",children:[]}]
fn total(tree:Tree)->int:
  if length(tree.children)==0:length(tree.name)
  else:length(tree.name)+total(tree.children[0])+total(tree.children[1])
]=])
COMPILE(tree "${TREE}enum Forest:\n  End\n  More:\n    roots:[Tree]\n    next:Forest\nfn make()->Tree:\n  shared=Tree {name:\"\",children:leaves()}\n  Tree {name:\"x\",children:[shared,shared]}\nfn main()->int:\n  inner=Forest.More {roots:[make()],next:Forest.End}\n  root=Forest.More {roots:[],next:inner}\n  match root:\n    Forest.End:0\n    Forest.More {next}:\n      match next:\n        Forest.End:0\n        Forest.More {roots}:total(roots[0])\n")
EXPECT(tree 5)
COMPILE(operations "${TREE}fn main()->String:\n  values=leaves()\n  combined=concat(slice(values,0,1),slice(values,1,2))\n  root=Tree {name:\"x\",children:combined}\n  root.children[0].name+root.children[1].name\n")
EXPECT(operations "é👩")
COMPILE(mutual [=[type Node:
  name:String
  links:[Link]
enum Link:
  End
  More:
    node:Node
fn make()->Node:
  Node {name:"root",links:[Link.End,Link.More {node:Node {name:"é",links:[]}}]}
fn main()->String:
  original=make()
  changed=original with {name:"new"}
  match changed.links[1]:
    Link.End:"bad"
    Link.More {node}:original.name+changed.name+node.name
]=])
EXPECT(mutual "rootnewé")
COMPILE(direct-list [=[enum List:
  Nil
  Cons:
    value:int
    next:List
enum Boxes:
  End
  More:
    values:[List]
    next:Boxes
fn sum(items:List)->int:
  match items:
    List.Nil:0
    List.Cons {value,next}:value+sum(next)
fn main()->int:
  list=List.Cons {value:20,next:List.Cons {value:1,next:List.Nil}}
  inner=Boxes.More {values:[list,list],next:Boxes.End}
  outer=Boxes.More {values:[],next:inner}
  match outer:
    Boxes.End:0
    Boxes.More {next}:
      match next:
        Boxes.End:0
        Boxes.More {values}:sum(values[0])+sum(values[1])
]=])
EXPECT(direct-list 42)
COMPILE(inactive "${PREFIX}fn main()->int:\n  score(Chain.End)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_FAIL_ALLOC_AT=1 "${OUTPUT_DIR}/inactive"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "0" OR NOT OUT STREQUAL "0\n" OR NOT ERR STREQUAL "")
  message(FATAL_ERROR "Inactive recursive record arrays allocated: ${STATUS}: ${OUT}${ERR}")
endif()
COMPILE(bounds "${PREFIX}fn main()->String:\n  chain=make()\n  match chain:\n    Chain.End:\"\"\n    Chain.Link {entries}:entries[3].name\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/bounds"
  RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
    NOT ERR MATCHES "BoundsFault" OR NOT ERR MATCHES "cleanup: live=0")
  message(FATAL_ERROR "Recursive record array fault cleanup: ${STATUS}: ${OUT}${ERR}")
endif()
foreach(FAIL_AT RANGE 1 60)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "KALANDRA_TEST_FAIL_ALLOC_AT=${FAIL_AT}"
    KALANDRA_TEST_REPORT_CLEANUP=1 "${OUTPUT_DIR}/returned"
    RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if("${STATUS}" STREQUAL "0")
    if(NOT OUT STREQUAL "88\n" OR NOT ERR STREQUAL "")
      message(FATAL_ERROR "Recursive record array result changed: ${OUT}${ERR}")
    endif()
  elseif(NOT "${STATUS}" STREQUAL "86" OR NOT OUT STREQUAL "" OR
      NOT ERR MATCHES "AllocationFault" OR NOT ERR MATCHES "cleanup: live=0")
    message(FATAL_ERROR "Recursive record array allocation failure leaked: ${STATUS}: ${OUT}${ERR}")
  endif()
endforeach()
