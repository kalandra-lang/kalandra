enum Choice:
  Some
fn descend(n:int,base:int)->fn(int)->int:
  match Choice.Some:
    Choice.Some:
      if n==0:
        fn(value):value+base
      else:
        descend(n-1,base+1)
fn main()->int:
  callback=descend
  result=callback(20,20)
  result(2)
