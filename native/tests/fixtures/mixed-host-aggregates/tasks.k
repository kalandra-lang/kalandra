type Batch:
  values:[i64]
  enabled:bool
fn make(n:i64)->Batch:
  return Batch {values:[n,n+1],enabled:true}
fn first(batch:Batch)->i64:
  return batch.values[0]
fn work(batch:Batch)->i64:
  total=0
  for i in range(batch.values[0]):
    total=total+2
  return total
fn main()->i64 ! TaskError:
  saved=make(17)
  return try parallel:
    total=0
    for i in range(1000):
      batch=make(i%2)
      total=total+first(batch)
    value=try await spawn work(make(1000)) on devices.gpu()
    value+total+saved.values[0]+saved.values[1]+3
