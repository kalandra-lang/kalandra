enum Choice:
  Empty
  Some
fn count(n:int)->int:
  match Choice.Some:
    Choice.Empty:0
    Choice.Some:
      if n==0:
        22
      else:
        count(n-1)+1
fn main()->int:
  callback=count
  callback(20)
