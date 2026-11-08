fn replace(mut values:[bool])->bool:
  saved=values
  values[0]=if true:
    values=[true,false]
    false
  else:true
  return not saved[0] and values[0]
fn main()->bool:
  original=[true]
  result=replace(original)
  return result and not original[0]
