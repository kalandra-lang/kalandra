error InputError:
  Invalid:
    value: int

fn checked(value: int) -> int ! InputError:
  if value == 0:
    fail InputError.Invalid {value: 42}
  value * 2

fn apply(value: int, transform: fn(int) -> int ! InputError) -> int ! InputError:
  try transform(value)

export fn main() -> int:
  transform = checked
  apply(0, transform) catch fn(error):
    match error:
      InputError.Invalid {value}: value
