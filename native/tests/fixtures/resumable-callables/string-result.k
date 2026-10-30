fn append(value:String)->String:
  value+"!"
fn run(value:String)->String ! TaskError:
  try parallel:
    try await spawn append(value)
fn main()->int ! TaskError:
  callback=run
  text=try callback("é")
  length(text)+40
