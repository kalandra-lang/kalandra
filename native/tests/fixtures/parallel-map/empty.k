fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  input:[int]=[]
  values=try parallelMap(input,double)
  length(values)+42
