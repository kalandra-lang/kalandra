fn replace(mut values:[Bool])->Bool:
  saved=values
  values[0]=if true:
    values=[true,false]
    false
  else:true
  not saved[0] and values[0]
fn main()->Bool:
  original=[true]
  result=replace(original)
  result and not original[0]
