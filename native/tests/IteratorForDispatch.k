from "std/iter" import Iterator, iter, close, range as steps

type Answer:
  value:i64

fn worker(own cursor:Iterator[i64,never],limit:i64)->Answer:
  if limit==0:
    close(cursor)
    return Answer {value:0}
  count=0
  total=0
  for value in cursor:
    total=total+value
    count=count+1
    if count==limit:
      break
  return Answer {value:total}

fn main()->i64:
  return worker(iter(steps(3)),10).value
