error InputError:
  Invalid:
    value: i64

fn checked(value: i64) -> i64 ! InputError:
  if value == 0:
    fail InputError.Invalid {value: 42}
  return value * 2

fn apply(value: i64, transform: fn(i64) -> i64 ! InputError) -> i64 ! InputError:
  return try transform(value)

export fn main() -> i64:
  transform = checked
  return apply(0, transform) catch fn(error):
    return match error:
      InputError.Invalid {value}: value
