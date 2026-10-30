fn worker(a:int,b:int)->int:
  a+b
fn main()->[int] ! TaskError:
  try parallelMap([21],worker)
