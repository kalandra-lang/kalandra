"""Portable resumable-call ownership and recursion qualification."""
SOURCE = '''from "std/time" import sleep, nanoseconds, TimeError
from "std/io" import println, IoError
from "std/fmt" import format
error E:
  Bad:
    text:String
    values:[i64]
fn walk(n:i64)->i64 ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(1)))
    return 17
  return 1+try walk(n-1)
fn tail(n:i64)->i64 ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(1)))
    return 17
  return try tail(n-1)
fn even(n:i64)->bool ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(1)))
    return true
  return try odd(n-1)
fn odd(n:i64)->bool ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(1)))
    return false
  return try even(n-1)
fn failDeep(n:i64)->i64 ! (E | TimeError):
  if n==0:
    try sleep(nanoseconds(u64(1)))
    fail E.Bad {text:"é!",values:[20,20]}
  return try failDeep(n-1)
fn values(n:i64)->[i64] ! TimeError:
  if n==0:
    try sleep(nanoseconds(u64(1)))
    return [20,22]
  return try values(n-1)
fn main()->unit ! (TimeError | IoError):
  try println(format("{}",try walk(2000)))
  try println(format("{}",try tail(10000)))
  try println(format("{}",try even(2000)))
  number=failDeep(2000) catch fn(error):
    return match error:
      E.Bad {text,values}:length(text)+values[0]+values[1]
      TimeError.Unavailable:0
      TimeError.OutOfRange:0
  try println(format("{}",number))
  total:i64=0
  for i in range(20):
    found=try values(64)
    total=total+found[0]+found[1]
  try println(format("{}",total))
  return ()
'''
EXPECTED = '2017\n17\ntrue\n42\n840\n'
