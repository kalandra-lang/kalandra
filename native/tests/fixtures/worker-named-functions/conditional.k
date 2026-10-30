fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn run(flag:Bool)->int ! TaskError:
  transform=if flag:
    alias=double
    alias
  else:
    alias=triple
    alias
  try parallel:
    try await spawn apply(14,transform)
fn main()->int ! TaskError:
  a=try run(false)
  b=try run(true)
  a+b-28
