fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn main()->int ! TaskError:
  try parallel:
    try await spawn apply(21,double)
