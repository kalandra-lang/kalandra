fn append(value:String)->String:
  return value+"!"
fn run(value:String)->String ! TaskError:
  return try parallel:
    try await spawn append(value)
fn main()->i64 ! TaskError:
  callback=run
  text=try callback("é")
  return length(text)+40
