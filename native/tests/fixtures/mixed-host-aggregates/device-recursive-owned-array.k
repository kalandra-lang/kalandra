fn walk(n:int, values:[int])->[int]:
  if n==0:
    values
  else:
    copyOfValues=copy(values)
    copyOfValues[0]=n
    returned=walk(n-1,copyOfValues)
    [values[0]+returned[0]]
fn entry(n:int)->[int]:
  walk(n,[7])
fn main()->int ! TaskError:
  try parallel:
    returned=try await spawn entry(100) on devices.gpu()
    returned[0]
