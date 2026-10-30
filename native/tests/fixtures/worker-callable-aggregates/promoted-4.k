fn double(value:int)->int:
  value*2
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
type Box:
  f:fn(int)->int
fn worker(box:Box)->int:
  box.f(21)
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker(Box {f:double})
