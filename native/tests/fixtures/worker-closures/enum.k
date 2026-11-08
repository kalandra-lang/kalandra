enum Item:
  Data:
    text:String
    values:[i64]
  Empty
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  item=Item.Data {text:"é"+"!",values:[20,20]}
  f=fn(value:i64):
    return match item:
      Item.Data {text,values}:length(text)+values[0]+values[1]+value
      Item.Empty:0
  return try parallel:
    try await spawn apply(0,f)
