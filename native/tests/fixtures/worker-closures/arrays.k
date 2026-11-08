fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  texts=["é","🙂"]
  flags=[true,false]
  f=fn(value:i64):
    return if flags[0]:value+length(texts[0])+length(texts[1])
    else:0
  return try parallel:
    try await spawn apply(40,f)
