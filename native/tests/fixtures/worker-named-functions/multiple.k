fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn combine(value:int,first:fn(int)->int,second:fn(int)->int)->int:
  first(value)+second(value)
fn main()->int ! TaskError:
  try parallel:
    try await spawn combine(21,double,triple)-63
