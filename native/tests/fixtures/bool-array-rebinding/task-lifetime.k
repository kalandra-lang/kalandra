type LoopResult:
  value:int
fn read(values:[Bool])->int:
  if length(values)==2:7
  else:
    if values[0]:1
    else:4
fn probe(n:int,faultAt:int)->LoopResult ! TaskError:
  values=[true,true]
  saved=values
  i=0
  total=0
  while i<n:
    value=try parallel:
      old=spawn read(values)
      values=if i%2==0:[true,false,true]
      else:[false]
      try await old
    total=total+value
    if i==faultAt:
      zero=0
      total=total+1/zero
    i=i+1
  LoopResult {value:read(saved)+total+read(values)+length(values)}
fn main()->int:
  0
