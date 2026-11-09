type Row:
  text:String
  values:[i64]
fn main()->i64:
  row=Row {text:"é"+"!",values:[20,20]}
  values=map([0],fn(value):
    count=length(row.text)
    return row.values[0]+row.values[1]+count+value
  )
  return values[0]
