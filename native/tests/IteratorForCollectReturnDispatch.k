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
    yield Answer {value:value}
    total=total+value
    count=count+1
    if count==limit:
      return Answer {value:total}
  values=result catch fn(error):
    return match error:
      Problem.Bad {code,message}:
        if message=="error":[Answer {value:-code}]
        else:[Answer {value:-i64(1000)}]
  sum=0
  for value in values:
    sum=sum+value.value
  return Answer {value:sum}

fn main()->unit:
  return ()
