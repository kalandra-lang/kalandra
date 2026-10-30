fn make(offset:int)->[fn(int)->int]:
  [fn(value):value+offset,fn(value):value*offset]
fn main()->int:
  callbacks=make(20)
  callbacks[0](2)+callbacks[1](1)
