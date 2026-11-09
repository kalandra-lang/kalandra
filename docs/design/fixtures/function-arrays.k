fn make(offset:i64)->[fn(i64)->i64]:
  return [fn(value):return value+offset,fn(value):return value*offset]
fn main()->i64:
  callbacks=make(20)
  return callbacks[0](2)+callbacks[1](1)
