fn read(values:[Bool])->Bool:
  values[0]
fn main()->int ! TaskError:
  values=[true,false]
  saved=values
  i=0
  total=0
  while i<100:
    old=try parallel:
      task=spawn read(values)
      values=try parallelMap(generate(i%17+1,fn(j):j),fn(j):j%2==0)
      values[0]=false
      try await task
    if old:
      total=total+1
    total=total+length(values)
    i=i+1
  if saved[0] and not saved[1]:total
  else:0
