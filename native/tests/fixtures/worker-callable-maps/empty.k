fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  inputs:[int]=[]
  f=double
  try parallel:
    values=try await spawn map(inputs,f)
    length(values)+42
