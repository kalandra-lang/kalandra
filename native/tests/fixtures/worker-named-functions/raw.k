fn double(value:int)->int:
  value*2
fn wrong(value:float64)->float64:
  value*3
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn main()->int ! TaskError:
  incompatible=wrong
  try parallel:
    try await spawn apply(21,double)
