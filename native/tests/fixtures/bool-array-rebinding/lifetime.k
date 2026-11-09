fn choose(index:i64)->[bool]:
  return if index%2==0: [true,false,true]
  else: [false]
fn alternate(index:i64)->[bool]:
  return choose(index)
fn run(count:i64)->i64:
  values=[true,true]
  saved=values
  index=0
  total=0
  while index<count:
    values=choose(index)
    total=total+length(values)
    index=index+1
  marker=if saved[0] and saved[1]:11
  else:0
  return total+length(values)+marker
fn main()->i64:
  return run(10000)
