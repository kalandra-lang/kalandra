type Row:
  text:String
  values:[i64]
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn main()->i64 ! TaskError:
  rows=[Row {text:"é"+"!",values:[20,20]}]
  f=fn(value:i64):return length(rows[0].text)+rows[0].values[0]+rows[0].values[1]+value
  return try parallel:
    work=spawn apply(0,f)
    a=try await work
    b=try await work
    a+b-42
