fn entry(n:i64)->i64:
  values=[n]
  cell=tensor(values,[])
  noValues=slice(values,0,0)
  empty=tensor(noValues,[0,n])
  dims=shape(empty)
  return cell[]+dims[1]+length(array(empty))+length(shape(cell))
fn main()->i64 ! TaskError:
  return try parallel:
    zero=spawn entry(0) on devices.gpu()
    four=spawn entry(4) on devices.gpu()
    (try await zero)+(try await four)
