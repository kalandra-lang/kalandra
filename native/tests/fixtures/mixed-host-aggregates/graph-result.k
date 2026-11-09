enum List:
  End
  Next:
    value:i64
    tail:List
fn make(n:i64)->List:
  tail=List.Next {value:7,tail:List.End}
  return List.Next {value:n,tail:tail}
fn head(list:List)->i64:
  return match list:
    List.End:0
    List.Next {value,tail}:value
fn score(list:List)->i64:
  return match list:
    List.End:0
    List.Next {value,tail}:value+head(tail)
fn main()->i64 ! TaskError:
  return try parallel:
    list=try await spawn make(11) on devices.gpu()
    score(list)
