fn double(value:int)->int:
  value*2
fn apply(value:int,transform:fn(int)->int)->int:
  transform(value)
fn worker(values:[fn(int)->int])->int:
  values[0](21)
fn main()->int ! TaskError:
  try parallel:
    try await spawn worker([double])
