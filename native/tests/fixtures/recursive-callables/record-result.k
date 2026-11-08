type Row:
  text:String
  values:[i64]
enum Choice:
  Some
fn descend(n:i64)->Row:
  return match Choice.Some:
    Choice.Some:
      if n==0:
        Row {text:"é"+"!",values:[20,20]}
      else:
        descend(n-1)
fn main()->i64:
  callback=descend
  row=callback(20)
  return row.values[0]+row.values[1]+length(row.text)
