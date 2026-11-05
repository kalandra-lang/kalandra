enum Tree:
  End
  Node:
    left:[int]
    right:[int]
    tail:Tree
fn convert(original:Tree)->Tree:
  original
fn inspect(tree:Tree)->int:
  match tree:
    Tree.End:0
    Tree.Node {left,right,tail}:
      match tail:
        Tree.End:0
        Tree.Node {left:original,right:other,tail:unused}:
          left[0]*100+right[0]*10+original[0]+other[0]
fn main()->int ! TaskError:
  values=[1,2,3]
  tail=Tree.Node {left:values,right:values,tail:Tree.End}
  input=Tree.Node {left:values,right:values,tail:tail}
  try parallel:
    returned=try await spawn convert(input) on devices.gpu()
    values[0]=9
    inspect(returned)
