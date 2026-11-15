from "std/iter" import Iterator, next, close, iter, range as steps
from "std/option" import Option

type Answer:
  value:i64

fn worker(own cursor:Iterator[i64,never],limit:i64)->Answer:
  count=0
  total=0
  while count<limit:
    match next(cursor):
      Option.None:break
      Option.Some {value}:
        total=total+value
        count=count+1
        ()
  close(cursor)
  return Answer {value:total}

fn main()->i64:
  return worker(iter(steps(3)),10).value
