type Row:
  text:String
  values:[int]
fn main()->int:
  row=Row {text:"é"+"!",values:[20,20]}
  values=map([0],fn(value):
    count=length(row.text)
    row.values[0]+row.values[1]+count+value
  )
  values[0]
