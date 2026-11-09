error E:
  Retry:
    callback:fn(i64)->i64
fn failer(offset:i64)->(fn(i64)->i64) ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:i64):return numbers[0]+value}
fn recover()->[fn(i64)->i64] ! TaskError:
  return try parallel:
    work=spawn failer(40)
    first=try (await work catch fn(error):
      return match error:
        E.Retry {callback}:callback
        _:fail error
    )
    second=try (await work catch fn(error):
      return match error:
        E.Retry {callback}:callback
        _:fail error
    )
    [first,second]
fn main()->i64 ! TaskError:
  callbacks=try recover()
  return callbacks[0](1)+callbacks[1](1)-40
