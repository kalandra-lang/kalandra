enum List:
  End
  Next:
    values:[int]
    tail:List
fn extend(n:int, tail:List)->List:
  if n==0:
    tail
  else:
    List.Next {values:[n],tail:extend(n-1,tail)}
fn score(list:List)->int:
  match list:
    List.End:0
    List.Next {values,tail}:values[0]+score(tail)
fn main()->int ! TaskError:
  captured=[1]
  tail=List.Next {values:captured,tail:List.End}
  try parallel:
    returned=try await spawn extend(100,tail) on devices.gpu()
    captured[0]=9
    score(returned)
