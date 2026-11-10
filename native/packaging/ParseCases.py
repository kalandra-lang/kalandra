"""Public parsing application shared by installed and cross-target qualification."""
SOURCE = '''from "std/parse" import parseI8, parseI16, parseI32, parseI64, parseU8, parseU16, parseU32, parseU64, parseF32, parseF64, parseBool, ParseError
from "std/io" import println, IoError
from "std/fmt" import format
fn classify(text:String)->String ! ParseError:
  number=try parseU8(text)
  return format("{}",number)
fn main()->unit ! (ParseError | IoError):
  a=try parseI8("-128")
  b=try parseI16("-32768")
  c=try parseI32("-2147483648")
  d=try parseI64("-9223372036854775808")
  e=try parseU8("255")
  f=try parseU16("65535")
  g=try parseU32("4294967295")
  h=try parseU64("18446744073709551615")
  x=try parseF32("16777217")
  y=try parseF64("9007199254740993")
  z=try parseBool("true")
  try println(format("{}:{}:{}:{}",a,b,c,d))
  try println(format("{}:{}:{}:{}",e,f,g,h))
  try println(format("{}:{}:{}",x==16777216,y==9007199254740992,z))
  for text in ["12x","256","12"]:
    result=classify(text) catch fn(error):
      return match error:
        ParseError.Invalid {offset}:format("invalid:{}",offset)
        ParseError.OutOfRange:"range"
    try println(result)
  return ()
'''
EXPECTED = '-128:-32768:-2147483648:-9223372036854775808\n255:65535:4294967295:18446744073709551615\ntrue:true:true\ninvalid:2\nrange\n12\n'
