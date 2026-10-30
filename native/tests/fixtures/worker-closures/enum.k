enum Item:
  Data:
    text:String
    values:[int]
  Empty
fn apply(value:int,f:fn(int)->int)->int:
  f(value)
fn main()->int ! TaskError:
  item=Item.Data {text:"é"+"!",values:[20,20]}
  f=fn(value:int):
    match item:
      Item.Data {text,values}:length(text)+values[0]+values[1]+value
      Item.Empty:0
  try parallel:
    try await spawn apply(0,f)
