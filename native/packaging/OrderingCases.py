"""Portable ordering and stable comparator sorting conformance programs."""
DECLARATIONS = '''from "std/cmp" import Order, Ordering, compare, totalCompare, sortedBy, sortBy
type SortPlayer:
  score:i64
  id:i64
impl Order for SortPlayer:
  fn compare(left:Self,right:Self)->Ordering:
    return compare(left.score,right.score)
fn byScore(left:SortPlayer,right:SortPlayer)->Ordering:
  return compare(left,right)
'''
DEVICE_CASES = {
 'stable-records':(DECLARATIONS+'''fn orderingRun()->i64:
  original=[SortPlayer {score:2,id:0},SortPlayer {score:1,id:1},SortPlayer {score:2,id:2},SortPlayer {score:1,id:3}]
  result=sortedBy(original,byScore)
  if result[0].id==1 and result[1].id==3 and result[2].id==0 and result[3].id==2:
    if original[0].id==0 and original[1].id==1:
      return 42
  return 0
''',42),
 'inplace-alias':(DECLARATIONS+'''fn orderingRun()->i64:
  values=[4,1,3,1,2]
  alias=values
  sorted=sortBy(values,compare[i64])
  if values==[1,1,2,3,4] and alias==values and sorted==values:
    sortBy(values,fn(a:i64,b:i64):return compare(b,a))
    if alias==[4,3,2,1,1]:
      bits=[true,false,true,false]
      sortBy(bits,compare[bool])
      if bits==[false,false,true,true]:
        return 42
  return 0
''',42),
 'empty-and-equal':(DECLARATIONS+'''fn orderingRun()->i64:
  empty:[i64]=[]
  result=sortedBy(empty,compare[i64])
  sortBy(empty,compare[i64])
  singleton=[42]
  same=sortBy(singleton,compare[i64])
  equal=sortedBy([SortPlayer {score:1,id:0},SortPlayer {score:1,id:1},SortPlayer {score:1,id:2}],byScore)
  if length(result)==0 and same[0]==42 and equal[0].id==0 and equal[1].id==1 and equal[2].id==2:
    return 42
  return 0
''',42),
}
for numeric in ['f32','f64']:
 DEVICE_CASES['float-'+numeric]=(DECLARATIONS+f'''fn orderingRun()->i64:
  zero:{numeric}=0.0
  negative:{numeric}=-zero
  raw=zero/zero
  nan=if totalCompare(raw,zero)==Ordering.Less:-raw
  else:raw
  infinity:{numeric}=1.0/zero
  if totalCompare(negative,zero)!=Ordering.Less or totalCompare(infinity,nan)!=Ordering.Less:
    return 0
  if totalCompare(nan,nan)!=Ordering.Equal or nan==nan or negative!=zero:
    return 0
  values=[nan,infinity,zero,negative,-infinity,{numeric}(2),{numeric}(-2)]
  result=sortBy(values,totalCompare[{numeric}])
  if result[0]!=-infinity or result[1]!={numeric}(-2) or result[4]!={numeric}(2) or result[5]!=infinity:
    return 0
  if totalCompare(result[2],negative)==Ordering.Equal and totalCompare(result[3],zero)==Ordering.Equal and result[6]!=result[6]:
    return 42
  return 0
''',42)
HOST_CASES = {
 'strings':('''from "std/cmp" import compare, sortedBy, Ordering
fn main()->unit ! (IoError):
  values=sortedBy(["z","é","é","a","","😀"],compare[String])
  try println(format("{}|{}|{}|{}|{}|{}",values[0],values[1],values[2],values[3],values[4],values[5]))
  try println(format("{}:{}",compare("a","aa")==Ordering.Less,compare("é","é")==Ordering.Greater))
  return ()
''','|a|é|z|é|😀\ntrue:true\n'),
}

for numeric in ['i8','i16','i32','i64','u8','u16','u32','u64']:
 DEVICE_CASES['integer-'+numeric]=(DECLARATIONS+f'''fn orderingRun()->i64:
  values:[{numeric}]=[{numeric}(3),{numeric}(0),{numeric}(2),{numeric}(2)]
  sortBy(values,compare[{numeric}])
  if values[0]=={numeric}(0) and values[1]=={numeric}(2) and values[2]=={numeric}(2) and values[3]=={numeric}(3):
    return 42
  return 0
''',42)

HOST_CASES['effects']=('''from "std/cmp" import compare,sortedBy,Ordering
fn trace(text:String)->unit:
  println(text) catch fn(error):return ()
  return ()
fn input()->[i64]:
  trace("input")
  return [3,1,2]
fn chosen(a:i64,b:i64)->Ordering:
  trace(format("{}:{}",a,b))
  return compare(a,b)
fn choose()->fn(i64,i64)->Ordering:
  trace("choose")
  return chosen
fn main()->unit ! (IoError):
  result=sortedBy(input(),choose())
  try println(format("{}:{}:{}",result[0],result[1],result[2]))
  return ()
''','input\nchoose\n3:1\n1:2\n3:2\n1:2:3\n')

DEVICE_CASES['captured-comparator']=(DECLARATIONS+'''fn orderingRun()->i64:
  direction:i64=-1
  comparator=fn(a:i64,b:i64):return compare(a*direction,b*direction)
  direction=1
  values=sortedBy([1,3,2],comparator)
  if values[0]==3 and values[1]==2 and values[2]==1:
    return 42
  return 0
''',42)

DEVICE_CASES['dynamic-comparator']=(DECLARATIONS+'''fn ascending(a:i64,b:i64)->Ordering:
  return compare(a,b)
fn descending(a:i64,b:i64)->Ordering:
  return compare(b,a)
fn dispatch(values:[i64],comparator:fn(i64,i64)->Ordering)->[i64]:
  return sortedBy(values,comparator)
fn orderingRun()->i64:
  first=dispatch([3,1,2],ascending)
  second=dispatch([1,3,2],descending)
  if first[0]==1 and first[2]==3 and second[0]==3 and second[2]==1:
    return 42
  return 0
''',42)

# Independent rank counting verifies stability without implementing a second sort.
DEVICE_CASES['stable-ranks']=(DECLARATIONS+'''fn orderingRun()->i64:
  original=generate(65,fn(i):return SortPlayer {score:(i*31+7)%17,id:i})
  values=sortedBy(original,byScore)
  index:i64=0
  while index<length(values):
    expected:i64=0
    other:i64=0
    while other<length(original):
      if original[other].score<values[index].score:
        expected=expected+1
      else:
        if original[other].score==values[index].score and original[other].id<values[index].id:
          expected=expected+1
      other=other+1
    if expected!=index or original[index].id!=index:
      return 0
    index=index+1
  return 42
''',42)
