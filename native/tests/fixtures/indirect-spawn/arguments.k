fn double(value:i64)->i64:
  return value*2
type Input:
  value:i64
fn combine(values:[i64],label:String,active:bool,item:Input,f:fn(i64)->i64)->[i64]:
  return if active:[f(values[0])+length(label)+item.value]
  else:[0]
fn main()->i64 ! TaskError:
  function=combine
  values=try parallel:
    try await spawn function([19],"hi",true,Input {value:2},double)
  return values[0]
