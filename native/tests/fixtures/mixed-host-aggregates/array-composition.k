enum Node:
  End
  Branch:
    value:i64
    children:[Node]
fn score(node:Node)->i64:
  return match node:
    Node.End:1
    Node.Branch {value,children}:value+length(children)
fn work(n:i64)->i64:
  source=generate(n,fn(i):return Node.Branch {value:i,children:[Node.End]})
  mapped=map(source,fn(node):return Node.Branch {value:score(node),children:[node]})
  picked=for i in range(length(mapped)):
    if i%2==0:
      yield mapped[i]
  scores=map(picked,fn(node):return score(node))
  return length(picked)+scores[0]+scores[length(scores)-1]
fn main()->i64 ! TaskError:
  return try parallel:
    try await spawn work(301) on devices.gpu()
