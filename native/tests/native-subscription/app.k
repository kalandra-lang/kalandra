from "sensor" import subscribe, unsubscribe, deliver, beginDelivery, finishDelivery, openCount, closeCount, Subscription
from "std/process" import ExitStatus

fn cap(value: i32, limit: i32) -> i32:
  return if value > limit: limit
  else: value

fn start(limit: i32) -> Subscription:
  return subscribe(fn(value: i32) -> i32: return cap(value, limit))

fn exercise() -> bool:
  a = start(80)
  b = start(120)
  if beginDelivery(0) == 0:
    return false
  unsubscribe(a)
  denied = deliver(0, 101)
  other = deliver(1, 101)
  pending = finishDelivery()
  return denied == -1 and other == 101 and pending == 80 and openCount() == 1

fn stopAt(value: i32, cutoff: i32) -> i32:
  return if value >= cutoff: -1
  else: value

fn selfDetachAt(cutoff: i32) -> bool:
  token = subscribe(fn(value: i32) -> i32: return stopAt(value, cutoff))
  if beginDelivery(0) == 0:
    return false
  result = finishDelivery()
  return result == -1 and deliver(0, 90) == -1

fn main() -> ExitStatus:
  if exercise() and openCount() == 0 and closeCount() == 2 and selfDetachAt(100) and closeCount() == 3:
    return ExitStatus {code: 0}
  return ExitStatus {code: 1}
