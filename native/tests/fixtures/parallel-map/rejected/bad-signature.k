fn worker(a:i64,b:i64)->i64:
  return a+b
fn main()->[i64] ! TaskError:
  return try parallelMap([21],worker)
