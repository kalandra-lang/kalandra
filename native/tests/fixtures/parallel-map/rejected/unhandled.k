fn double(value:int)->int:
  value*2
fn main()->int:
  values=parallelMap([21],double)
  values[0]
