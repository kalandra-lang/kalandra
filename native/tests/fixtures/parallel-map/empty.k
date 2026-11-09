fn double(value:i64)->i64:
  return value*2
fn main()->i64 ! TaskError:
  input:[i64]=[]
  values=try parallelMap(input,double)
  return length(values)+42
