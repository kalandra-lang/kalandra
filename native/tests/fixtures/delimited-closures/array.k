fn main()->int:
  offset:int=20
  callbacks:[fn(int)->int]=[
    fn(value):
      total=value+offset
      total
    ,fn(value):
      product=value*offset
      product
  ]
  callbacks[0](2)+callbacks[1](1)
