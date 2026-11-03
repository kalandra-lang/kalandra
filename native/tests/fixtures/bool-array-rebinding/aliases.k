fn run(flag:Bool)->int:
  values=[true,false]
  saved=values
  if flag:
    values=[false,true,false]
  else:
    values=[true]
  values[0]=true
  result=if saved[0] and not saved[1] and values[0]:length(values)
  else:1000
  while false:
    values=[false]
  result+length(values)
fn main()->int:
  run(true)+run(false)
