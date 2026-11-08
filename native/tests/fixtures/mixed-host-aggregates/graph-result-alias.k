enum Tree:
  End
  Node:
    left:[i64]
    right:[i64]
    tail:Tree
fn make(n:i64)->Tree:
  shared=generate(n,fn(i):return i+1)
  child=Tree.Node {left:shared,right:copy(shared),tail:Tree.End}
  return Tree.Node {left:shared,right:shared,tail:child}
fn inspect(tree:Tree)->i64:
  return match tree:
    Tree.End:0
    Tree.Node {left,right,tail}:
      match tail:
        Tree.End:0
        Tree.Node {left:child,right:independent,tail:unused}:
          left[0]*100+right[0]*10+child[0]+independent[0]
fn main()->i64 ! TaskError:
  return try parallel:
    root=try await spawn make(9000) on devices.gpu()
    match root:
      Tree.End:0
      Tree.Node {left,right,tail}:
        left[0]=9
        inspect(root)
