from "std/iter" import Iterator, close

error Problem:
  Bad:
    code:i64
    message:String

type Answer:
  value:i64

fn consume[E](own cursor:Iterator[i64,E],limit:i64)->i64 ! E:
  if limit==0:
    close(cursor)
    return 0
  count=0
  total=0
  return try for value in cursor:
    total=total+value
    count=count+1
    if count==limit:
      break total
  else:total

fn worker(own cursor:Iterator[i64,Problem],limit:i64)->Answer:
  result=consume(cursor,limit) catch fn(error):
    return match error:
      Problem.Bad {code,message}:
        if message=="error":-code
        else:-1000
  return Answer {value:result}

fn main()->unit:
  return ()
