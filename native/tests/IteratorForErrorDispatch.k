from "std/iter" import Iterator, close

error Problem:
  Bad:
    code:i64
    message:String

type Answer:
  value:i64

fn worker(own cursor:Iterator[i64,Problem],limit:i64)->Answer:
  if limit==0:
    close(cursor)
    return Answer {value:0}
  count=0
  total=0
  result=for value in cursor:
    total=total+value
    count=count+1
    if count==limit:
      break total
  else:total
  recovered=result catch fn(error):
    return match error:
      Problem.Bad {code,message}:
        if message=="error":-code
        else:-1000
  return Answer {value:recovered}

fn main()->unit:
  return ()
