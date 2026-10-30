fn double(value:int)->int:
  value*2
fn main()->int ! TaskError:
  values=try parallelMap([10,11],fn(value)->int ! TaskError:
    inner=try parallelMap([value],double)
    inner[0]
  )
  values[0]+values[1]
