error E:
  Retry:
    callback:fn(int)->int
fn failer(offset:int)->int ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:int):numbers[0]+value}
fn worker(offset:int)->(fn(int)->int) ! E:
  if offset==0:fail E.Retry {callback:fn(value:int):value}
  fn(value:int):offset+value
fn main()->int ! (E | TaskError):
  callback=try parallel:
    try await spawn worker(40)
  callback(2)
