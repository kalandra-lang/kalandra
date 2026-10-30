fn double(value:int)->int:
  value*2
fn worker(value:int)->int ! TaskError:
  result=try parallelMap([value],double)
  result[0]
fn main()->int ! TaskError:
  try parallel:
    first=spawn worker(10)
    second=spawn worker(11)
    (try await first)+(try await second)
