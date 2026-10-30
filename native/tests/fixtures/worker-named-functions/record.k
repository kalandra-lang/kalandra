type Row:
  text:String
  values:[int]
fn decorate(value:Row)->Row:
  value with {text:value.text+"!"}
fn apply(value:Row,transform:fn(Row)->Row)->Row:
  transform(value)
fn main()->int ! TaskError:
  row=try parallel:
    try await spawn apply(Row {text:"é",values:[20,20]},decorate)
  length(row.text)+row.values[0]+row.values[1]
