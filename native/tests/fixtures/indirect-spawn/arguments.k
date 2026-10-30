fn double(value:int)->int:
  value*2
type Input:
  value:int
fn combine(values:[int],label:String,active:Bool,item:Input,f:fn(int)->int)->[int]:
  if active:[f(values[0])+length(label)+item.value]
  else:[0]
fn main()->int ! TaskError:
  function=combine
  values=try parallel:
    try await spawn function([19],"hi",true,Input {value:2},double)
  values[0]
