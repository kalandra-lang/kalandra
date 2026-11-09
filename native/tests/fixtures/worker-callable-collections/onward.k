fn make(offset:i64)->fn(i64)->i64:
  numbers=[offset]
  return fn(value:i64):return numbers[0]+value
fn main()->i64 ! TaskError:
  original=try parallel:
    try await spawn map([40],make)
  selected=try parallel:
    try await spawn slice(original,0,1)
  combined=try parallel:
    try await spawn concat(selected,selected)
  result=try parallel:
    try await spawn map(combined,fn(callback):return callback(1))
  return result[0]+result[1]-40
