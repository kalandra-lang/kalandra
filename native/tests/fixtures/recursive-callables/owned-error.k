error E:
  Bad:
    text:String
    values:[int]
fn descend(n:int,text:String)->int ! E:
  if n==0:fail E.Bad {text:text,values:[20,20]}
  words=text+""
  try descend(n-1,words)
fn main()->int:
  callback=descend
  callback(20,"é"+"!") catch fn(error):
    match error:
      E.Bad {text,values}:values[0]+values[1]+length(text)
