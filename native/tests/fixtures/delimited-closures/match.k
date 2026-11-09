enum Choice:
  Empty
  Some:
    value:i64
fn main()->i64:
  values=map([Choice.Empty,Choice.Some {value:22}],fn(choice):
    return match choice:
      Choice.Empty:
        base:i64=20
        base
      Choice.Some {value}:value
  )
  return values[0]+values[1]
