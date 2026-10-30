fn double(value:int)->int:
  value*2
fn twice(value:float64)->float64:
  value*2
fn apply(value:int,first:fn(int)->int,second:fn(float64)->float64)->int:
  first(value)+int(second(float64(value)))-42
fn main()->int ! TaskError:
  try parallel:
    try await spawn apply(21,double,twice)
