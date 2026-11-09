error E:
  Bad:
    text:String
    values:[i64]
fn descend(n:i64,text:String)->i64 ! E:
  if n==0:fail E.Bad {text:text,values:[20,20]}
  words=text+""
  return try descend(n-1,words)
fn main()->i64:
  callback=descend
  return callback(20,"é"+"!") catch fn(error):
    return match error:
      E.Bad {text,values}:values[0]+values[1]+length(text)
