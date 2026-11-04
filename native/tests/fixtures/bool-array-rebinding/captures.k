fn main()->[Bool]:
  original=[true,false]
  f=fn()->[Bool]:
    original=sorted(original)
    original[0]=true
    original
  first=f()
  first[1]=false
  second=f()
  [first[1],second[1],original[0],original[1]]
