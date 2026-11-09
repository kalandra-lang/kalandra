type Row:
  text:String
  values:[i64]
fn decorate(value:Row)->Row:
  return value with {text:value.text+"!"}
fn apply(value:Row,transform:fn(Row)->Row)->Row:
  return transform(value)
fn main()->i64 ! TaskError:
  row=try parallel:
    try await spawn apply(Row {text:"é",values:[20,20]},decorate)
  return length(row.text)+row.values[0]+row.values[1]
