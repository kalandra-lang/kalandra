fn double(value:int)->int:
  value*2
fn triple(value:int)->int:
  value*3
fn apply(value:int,transform:fn(int)->int)->int ! TaskError:
  try parallel:
    work=spawn double(value)
    result=try await work
    transform(result)
fn main()->int ! TaskError:
  try parallel:
    try await spawn apply(7,triple)
