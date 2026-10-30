type Config:
  values:[int]
  text:String
fn compute(value:int,config:Config)->int:
  value+config.values[0]+length(config.text)
fn make(config:Config)->(fn(int)->int ! TaskError):
  fn(value:int)->int ! TaskError:
    try parallel:
      try await spawn compute(value,config)
fn main()->int ! TaskError:
  f=make(Config {values:[20],text:"é🙂"})
  try parallel:
    try await spawn f(20)
