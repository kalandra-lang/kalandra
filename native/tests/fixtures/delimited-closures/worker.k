type Row:
  text:String
  values:[int]
fn worker(input:int)->[Row]:
  map([input],fn(value):
    words="é"+"!"
    numbers:[int]=[value]
    Row {text:words,values:numbers}
  )
fn main()->int ! TaskError:
  try parallel:
    task=spawn worker(40)
    first=try await task
    second=try await task
    first[0].values[0]+length(second[0].text)
