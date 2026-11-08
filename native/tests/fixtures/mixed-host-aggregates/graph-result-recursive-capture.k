enum List:
  End
  Next:
    values:[i64]
    tail:List
fn extend(n:i64, tail:List)->List:
  return if n==0:
    tail
  else:
    List.Next {values:[n],tail:extend(n-1,tail)}
fn score(list:List)->i64:
  return match list:
    List.End:0
    List.Next {values,tail}:values[0]+score(tail)
fn main()->i64 ! TaskError:
  captured=[1]
  tail=List.Next {values:captured,tail:List.End}
  return try parallel:
    returned=try await spawn extend(100,tail) on devices.gpu()
    captured[0]=9
    score(returned)
