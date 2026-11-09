error E:
  Bad:
    text:String
    values:[i64]
fn descend(n:i64)->i64 ! E:
  if n==0:fail E.Bad {text:"é"+"!",values:[20,20]}
  return try descend(n-1)
fn worker(n:i64)->i64:
  callback=descend
  return callback(n) catch fn(error):
    return match error:
      E.Bad {text,values}:values[0]+values[1]+length(text)
fn main()->i64 ! TaskError:
  return try parallel:
    task=spawn worker(20)
    first=try await task
    second=try await task
    first+second-42
