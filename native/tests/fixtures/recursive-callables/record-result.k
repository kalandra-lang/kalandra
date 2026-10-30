type Row:
  text:String
  values:[int]
enum Choice:
  Some
fn descend(n:int)->Row:
  match Choice.Some:
    Choice.Some:
      if n==0:
        Row {text:"é"+"!",values:[20,20]}
      else:
        descend(n-1)
fn main()->int:
  callback=descend
  row=callback(20)
  row.values[0]+row.values[1]+length(row.text)
