enum Tree:
  End
  Node:
    left:[i64]
    right:[i64]
    tail:Tree
fn convert(original:Tree)->Tree:
  return match original:
    Tree.End:Tree.End
    Tree.Node {left,right,tail}:
      Tree.Node {left:left,right:copy(left),tail:original}
fn inspect(tree:Tree)->i64:
  return match tree:
    Tree.End:0
    Tree.Node {left,right,tail}:
      match tail:
        Tree.End:0
        Tree.Node {left:original,right:other,tail:unused}:
          left[0]*100+right[0]*10+original[0]+other[0]
fn main()->i64 ! TaskError:
  values=[1,2,3]
  input=Tree.Node {left:values,right:values,tail:Tree.End}
  return try parallel:
    returned=try await spawn convert(input) on devices.gpu()
    values[0]=9
    inspect(returned)
