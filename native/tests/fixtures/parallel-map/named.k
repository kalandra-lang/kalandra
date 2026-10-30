fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  values=try parallelMap([21],double)
  values[0]
