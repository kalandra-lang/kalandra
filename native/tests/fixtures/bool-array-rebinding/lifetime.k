fn choose(index:int)->[Bool]:
  if index%2==0: [true,false,true]
  else: [false]
fn alternate(index:int)->[Bool]:
  choose(index)
fn run(count:int)->int:
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
  total+length(values)+marker
fn main()->int:
  run(10000)
