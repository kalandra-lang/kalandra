fn main()->i64:
  offset:i64=20
  callbacks:[fn(i64)->i64]=[
    fn(value):
      total=value+offset
      return total
    ,fn(value):
      product=value*offset
      return product
  ]
  return callbacks[0](2)+callbacks[1](1)
