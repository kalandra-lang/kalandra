fn double(value:int)->int:
  value*2
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
error E:
  Bad:
    value:int
fn make(flag:Bool)->(fn(int)->int) ! E:
  if flag:fail E.Bad {value:1}
  double
fn main()->int ! (E | TaskError):
  factory=make
  f=try factory(false)
  try parallel:
    try await spawn apply(21,f)
