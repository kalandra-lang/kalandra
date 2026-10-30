fn double(value:int)->int:
  value*2
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
type Box:
  f:fn(int)->int
fn run(flag:Bool)->int ! TaskError:
  box=Box {f:double}
  f=if flag:double
  else:fn(value:int):box.f(value)
  try parallel:
    try await spawn apply(21,f)
fn main()->int ! TaskError:
  try run(true)
