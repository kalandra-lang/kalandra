"""Deterministic public time API coverage, including a suspending call chain."""
SOURCE = '''from "std/time" import now, elapsed, nanoseconds, milliseconds, asNanoseconds, asSeconds, sleep, Duration, TimeError
from "std/io" import println, IoError
from "std/fmt" import format
fn pause(duration:Duration)->unit ! TimeError:
  try sleep(duration)
  return ()
fn pure()->u64:
  maximum:u64=18446744073709551615
  return asNanoseconds(nanoseconds(maximum))
fn main()->unit ! (TimeError | IoError | TaskError):
  try println(format("{}",pure()))
  try println(format("{}",asNanoseconds(try milliseconds(u64(1500)))))
  try println(format("{}",asSeconds(nanoseconds(u64(1500000000)))==1.5))
  start=try now()
  duration=try milliseconds(u64(5))
  try pause(duration)
  try println(format("{}",asNanoseconds(try elapsed(start))>=asNanoseconds(duration)))
  try parallel:
    first=spawn pause(duration)
    second=spawn pause(nanoseconds(u64(0)))
    try await second
    try await first
  overflow=milliseconds(pure()) catch fn(error):
    match error:
      TimeError.Unavailable: ()
      TimeError.OutOfRange: ()
    return nanoseconds(u64(7))
  try println(format("{}",asNanoseconds(overflow)))
  return ()
'''
EXPECTED = '18446744073709551615\n1500000000\ntrue\ntrue\n7\n'
