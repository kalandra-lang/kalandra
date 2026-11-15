"""Portable static generics, traits, associated types, callbacks and ownership."""
SOURCE = '''from "std/io" import println, IoError
from "std/fmt" import format
from "std/buffer" import ByteBuffer, BufferError, byteBuffer, append, intoArray

type Pair[A, B]:
  first: A
  second: B

enum Option[T]:
  None
  Some:
    value: T

type Box[T]:
  value: T

trait Plus:
  fn add(left: Self, right: Self) -> Self

impl Plus for i64:
  fn add(left: Self, right: Self) -> Self:
    return left + right

impl Plus for f64:
  fn add(left: Self, right: Self) -> Self:
    return left + right

trait Read:
  type Item
  fn read(value: Self) -> Self.Item

impl[T] Read for Box[T]:
  type Item = T
  fn read(value: Self) -> Self.Item:
    return value.value

fn identity[T](value: T) -> T:
  return value

fn add[T: Plus](left: T, right: T) -> T:
  return Plus.add(left, right)

fn first[A, B](value: Pair[A, B]) -> A:
  return value.first

fn unwrap[T](value: Option[T], fallback: T) -> T:
  return match value:
    Option.None: fallback
    Option.Some {value}: value

fn read[T: Read](value: T) -> T.Item:
  return Read.read(value)

fn makeAdder[T: Plus](offset: T) -> fn(T) -> T:
  return fn(value: T): return Plus.add(value, offset)

fn forward[T](own value: T) -> T:
  return value

error Invalid:
  Negative

fn checked(value: i64) -> i64 ! Invalid:
  if value < 0:
    fail Invalid.Negative
  return value

fn apply[T, U, E](value: T, callback: fn(T) -> U ! E) -> U ! E:
  return try callback(value)

fn retainCallback[T, U, E](callback: fn(T) -> U ! E):
  return fn(value: T) -> U ! E: return try callback(value)

fn main() -> unit ! (IoError | BufferError):
  pair = Pair {first: 42, second: "retained"}
  some = Option.Some {value: 9}
  none: Option[i64] = Option.None
  callback = identity[i64]
  increment = makeAdder[i64](22)
  retained = retainCallback[i64, i64, Invalid](checked)
  expected = apply(-1, retained) catch fn(error): return 7
  buffer = try byteBuffer()
  try append(buffer, [40, 2])
  moved = forward(buffer)
  bytes = intoArray(moved)
  text = read(Box {value: "yes"})
  try println(format("{}:{}:{}:{}:{}:{}:{}:{}:{}", first(pair), callback(42), expected, unwrap(some, 0), unwrap(none, 12), increment(20), length(bytes), add(40.0, 2.0), text))
  return ()
'''
EXPECTED = '42:42:7:9:12:42:2:42:yes\n'

EQUALITY_DECLARATIONS = '''from "std/cmp" import Equal
type EqualityPoint:
  x:i64
  y:i64
impl Equal for EqualityPoint:
  fn equal(left:Self,right:Self)->bool:
    return left.x==right.x and left.y==right.y
fn equalityContains[T:Equal](values:[T],needle:T)->bool:
  for value in values:
    if value==needle:
      return true
  return false
'''
EQUALITY_DEVICE_CASES = {
    'records-arrays': (EQUALITY_DECLARATIONS+'''fn equalityRun()->i64:
  a=EqualityPoint {x:1,y:2}
  b=EqualityPoint {x:1,y:3}
  if a==a and a!=b and equalityContains([a,b],b) and [a,b]==[a,b] and [a]!=[b]:
    if [1,2]==[1,2] and [1,2]!=[1] and [true,false]==[true,false]:
      return 42
  return 0
''',42),
    'standard-values': ('''from "std/fs" import FileKind, OpenMode, FileInfo
from "std/option" import Option
from "std/iter" import Range
from "std/process" import ExitStatus
fn equalityRun()->i64:
  a=FileInfo {kind:FileKind.File,size:400}
  b=FileInfo {kind:FileKind.File,size:401}
  first:Option[i64]=Option.Some {value:42}
  second:Option[i64]=Option.None
  rangeA=Range {start:1,stop:10,step:2}
  rangeB=Range {start:1,stop:10,step:3}
  if FileKind.File==FileKind.File and FileKind.File!=FileKind.Other:
    if OpenMode.Read!=OpenMode.Write and a==a and a!=b:
      if first==first and first!=second and second==second:
        if rangeA==rangeA and rangeA!=rangeB:
          if ExitStatus {code:42}==ExitStatus {code:42}:
            return 42
  return 0
''',42),
}
numeric='from "std/cmp" import Equal\nfn equalityRun()->i64:\n'
for name in ('i8','i16','i32','i64','u8','u16','u32','u64','f32','f64'):
    numeric+=f'  if not (Equal.equal({name}(1),{name}(1)) and {name}(1)!={name}(2)):\n    return 0\n'
numeric+='''  if Equal.equal(true,true) and Equal.equal((),()) and true!=false:
    return 42
  return 0
'''
EQUALITY_DEVICE_CASES['primitives']=(numeric,42)
for name in ('f32','f64'):
    EQUALITY_DEVICE_CASES['ieee-'+name]=(f'''from "std/cmp" import Equal
fn equalityRun()->i64:
  nan={name}(0.0)/{name}(0.0)
  positive={name}(0.0)
  negative=-positive
  if nan!=nan and not Equal.equal(nan,nan) and positive==negative:
    if [nan]!=[nan] and [positive]==[negative]:
      return 42
  return 0
''',42)
EQUALITY_HOST_CASES = {
    'strings-options': ('''from "std/cmp" import Equal
from "std/option" import Option
fn main()->unit ! (IoError):
  some:Option[String]=Option.Some {value:"é"}
  none:Option[String]=Option.None
  try println(format("{}:{}:{}:{}",Equal.equal("é","é"),"é"!="é",
                     some==some,some!=none))
  try println(format("{}",["é","a"]==["é","a"]))
  return ()
''','true:true:true:true\ntrue\n'),
    'operand-order': ('''from "std/cmp" import Equal
fn emit(text:String)->unit:
  return println(text) catch fn(error):return ()
type EqualityMark:
  value:i64
impl Equal for EqualityMark:
  fn equal(left:Self,right:Self)->bool:
    emit("equal")
    return left.value==right.value
fn operand(value:i64)->EqualityMark:
  emit(format("arg{}",value))
  return EqualityMark {value:value}
fn main()->unit ! (IoError):
  try println(format("{}",operand(1)!=operand(2)))
  unused=false and (operand(3)==operand(4))
  try println(format("{}",unused))
  return ()
''','arg1\narg2\nequal\ntrue\nfalse\n'),
}
# Installed/cross-target consumers also execute the explicit equality contract.
SOURCE=EQUALITY_DECLARATIONS+SOURCE
SOURCE=SOURCE.replace('  return ()\n', '  try println(format("{}:{}",EqualityPoint {x:1,y:2}==EqualityPoint {x:1,y:2},equalityContains([1,2],2)))\n  return ()\n',1)
EXPECTED+='true:true\n'


EQUALITY_DEVICE_CASES['aliased-capability']=(EQUALITY_DECLARATIONS.replace('import Equal','import Equal as Same').replace('impl Equal','impl Same').replace('T:Equal','T:Same')+'''fn equalityRun()->i64:
  value=EqualityPoint {x:40,y:2}
  if equalityContains([value],value) and value==value:
    return 42
  return 0
''',42)
EQUALITY_DEVICE_CASES['recursive-method']=('''from "std/cmp" import Equal
type EqualityCounter:
  value:i64
impl Equal for EqualityCounter:
  fn equal(left:Self,right:Self)->bool:
    if left.value==0 or right.value==0:
      return left.value==right.value
    return EqualityCounter {value:left.value-1}==EqualityCounter {value:right.value-1}
fn equalityRun()->i64:
  if EqualityCounter {value:100}==EqualityCounter {value:100}:
    if EqualityCounter {value:100}!=EqualityCounter {value:99}:
      return 42
  return 0
''',42)
EQUALITY_DEVICE_CASES['record-fields']=('''from "std/cmp" import Equal
type EqualityRow:
  values:[i64]
impl Equal for EqualityRow:
  fn equal(left:Self,right:Self)->bool:
    return left.values==right.values
fn equalityRun()->i64:
  left=[EqualityRow {values:[1,2]},EqualityRow {values:[3]}]
  equal=[EqualityRow {values:[1,2]},EqualityRow {values:[3]}]
  other=[EqualityRow {values:[1,2]},EqualityRow {values:[4]}]
  if left==equal and left!=other:
    return 42
  return 0
''',42)


EQUALITY_HOST_CASES['argument-error']=('''from "std/cmp" import Equal
fn emit(text:String)->unit:
  return println(text) catch fn(error):return ()
error EqualityStop:
  Bad
type EqualityMark:
  value:i64
impl Equal for EqualityMark:
  fn equal(left:Self,right:Self)->bool:
    emit("equal")
    return true
fn first()->EqualityMark:
  emit("first")
  return EqualityMark {value:1}
fn second()->EqualityMark ! EqualityStop:
  emit("second")
  fail EqualityStop.Bad
fn attempt()->bool ! EqualityStop:
  return first()==try second()
fn main()->unit ! (IoError):
  result=attempt() catch fn(error):return false
  try println(format("{}",result))
  return ()
''','first\nsecond\nfalse\n')

EQUALITY_DEVICE_CASES['enum-payloads']=('''from "std/cmp" import Equal
enum EqualityReading:
  Missing
  Value:
    number:i64
impl Equal for EqualityReading:
  fn equal(left:Self,right:Self)->bool:
    return match left:
      EqualityReading.Missing:
        match right:
          EqualityReading.Missing:true
          EqualityReading.Value {number}:false
      EqualityReading.Value {number}:
        match right:
          EqualityReading.Missing:false
          EqualityReading.Value {number:other}:number==other
fn equalityRun()->i64:
  a=EqualityReading.Value {number:42}
  b=EqualityReading.Value {number:43}
  empty=EqualityReading.Missing
  if a==a and a!=b and empty==empty and a!=empty and empty!=a:
    return 42
  return 0
''',42)
