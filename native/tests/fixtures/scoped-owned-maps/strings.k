fn make(value: int) -> String:
  if value % 2 == 0:
    "even" + "!"
  else:
    "odd" + "!"
fn main() -> int ! TaskError:
  values = [1, 2, 3]
  index = 0
  total = 0
  while index < 3:
    names = try parallelMap(values, make)
    values = concat(values, [index])
    total = total + length(names[0]) + length(names[length(names) - 1])
    index = index + 1
  total
