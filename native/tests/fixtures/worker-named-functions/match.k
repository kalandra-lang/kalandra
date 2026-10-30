fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
enum Mode:
  Twice
  Thrice
fn run(mode:Mode)->int ! TaskError:
  transform=match mode:
    Mode.Twice:double
    Mode.Thrice:triple
  try parallel:
    try await spawn apply(14,transform)
fn main()->int ! TaskError:
  a=try run(Mode.Thrice)
  b=try run(Mode.Twice)
  a+b-28
