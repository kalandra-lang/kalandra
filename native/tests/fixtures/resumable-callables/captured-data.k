type Config:
  values:[i64]
  text:String
fn compute(value:i64,config:Config)->i64:
  return value+config.values[0]+length(config.text)
fn make(config:Config)->(fn(i64)->i64 ! TaskError):
  return fn(value:i64)->i64 ! TaskError:
    return try parallel:
      try await spawn compute(value,config)
fn main()->i64 ! TaskError:
  f=make(Config {values:[20],text:"é🙂"})
  return try parallel:
    try await spawn f(20)
