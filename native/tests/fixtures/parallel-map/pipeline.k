fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  values=try ([21] -> parallelMap(double))
  values[0]
