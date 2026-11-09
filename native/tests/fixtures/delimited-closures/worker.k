type Row:
  text:String
  values:[i64]
fn worker(input:i64)->[Row]:
  return map([input],fn(value):
    words="é"+"!"
    numbers:[i64]=[value]
    return Row {text:words,values:numbers}
  )
fn main()->i64 ! TaskError:
  return try parallel:
    task=spawn worker(40)
    first=try await task
    second=try await task
    first[0].values[0]+length(second[0].text)
