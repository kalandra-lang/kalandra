enum Choice:
  Empty
  Some:
    value:int
fn main()->int:
  values=map([Choice.Empty,Choice.Some {value:22}],fn(choice):
    match choice:
      Choice.Empty:
        base:int=20
        base
      Choice.Some {value}:value
  )
  values[0]+values[1]
