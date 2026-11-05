enum Node:
  End
  Branch:
    value:int
    children:[Node]
fn score(node:Node)->int:
  match node:
    Node.End:1
    Node.Branch {value,children}:value+length(children)
fn work(n:int)->int:
  source=generate(n,fn(i):Node.Branch {value:i,children:[Node.End]})
  mapped=map(source,fn(node):Node.Branch {value:score(node),children:[node]})
  picked=for i in range(length(mapped)):
    if i%2==0:
      yield mapped[i]
  scores=map(picked,fn(node):score(node))
  length(picked)+scores[0]+scores[length(scores)-1]
fn main()->int ! TaskError:
  try parallel:
    try await spawn work(301) on devices.gpu()
