type Batch:
  values:[int]
  enabled:Bool
fn make(n:int)->Batch:
  Batch {values:[n,n+1],enabled:true}
fn first(batch:Batch)->int:
  batch.values[0]
fn work(batch:Batch)->int:
  total=0
  for i in range(batch.values[0]):
    total=total+2
  total
fn main()->int ! TaskError:
  saved=make(17)
  try parallel:
    total=0
    for i in range(1000):
      batch=make(i%2)
      total=total+first(batch)
    value=try await spawn work(make(1000)) on devices.gpu()
    value+total+saved.values[0]+saved.values[1]+3
