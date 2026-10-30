error E:
  Retry:
    callback:fn(int)->int
fn failer(offset:int)->(fn(int)->int) ! E:
  numbers=[offset]
  fail E.Retry {callback:fn(value:int):numbers[0]+value}
fn recover()->[fn(int)->int] ! TaskError:
  try parallel:
    work=spawn failer(40)
    first=try (await work catch fn(error):
      match error:
        E.Retry {callback}:callback
        _:fail error
    )
    second=try (await work catch fn(error):
      match error:
        E.Retry {callback}:callback
        _:fail error
    )
    [first,second]
fn main()->int ! TaskError:
  callbacks=try recover()
  callbacks[0](1)+callbacks[1](1)-40
