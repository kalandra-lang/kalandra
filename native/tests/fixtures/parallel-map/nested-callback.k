fn double(value:int)->int:
  value*2
fn worker(value:int)->int ! TaskError:
  result=try parallelMap([value],double)
  result[0]
fn main()->int ! TaskError:
  values=try parallelMap([10,11],worker)
  values[0]+values[1]
