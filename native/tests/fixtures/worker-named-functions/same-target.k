fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn main()->int ! TaskError:
  transform=double
  try parallel:
    first=spawn apply(21,transform)
    second=spawn apply(21,transform)
    a=try await first
    b=try await second
    a+b-42
