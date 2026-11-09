fn negate(value:bool)->bool:
  return if value:false
  else:true
fn run(value:bool)->bool ! TaskError:
  return try parallel:
    try await spawn negate(value)
fn main()->i64 ! TaskError:
  callback=run
  selected=try callback(false)
  return if selected:42
  else:0
