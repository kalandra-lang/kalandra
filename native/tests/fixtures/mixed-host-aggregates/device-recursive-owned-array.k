fn walk(n:i64, values:[i64])->[i64]:
  return if n==0:
    values
  else:
    copyOfValues=copy(values)
    copyOfValues[0]=n
    returned=walk(n-1,copyOfValues)
    [values[0]+returned[0]]
fn entry(n:i64)->[i64]:
  return walk(n,[7])
fn main()->i64 ! TaskError:
  return try parallel:
    returned=try await spawn entry(100) on devices.gpu()
    returned[0]
