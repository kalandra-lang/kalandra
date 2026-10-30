fn double(value:int)->int:
  value*2
fn process(values:[int],callback:fn(int)->int)->[int] ! TaskError:
  try parallel:
    try await spawn map(values,callback)
fn main()->int ! TaskError:
  values=try process([21],double)
  values[0]
