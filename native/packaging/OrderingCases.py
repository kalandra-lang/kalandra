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
