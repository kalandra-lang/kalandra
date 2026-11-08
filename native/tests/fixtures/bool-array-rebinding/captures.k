fn main()->[bool]:
  original=[true,false]
  f=fn()->[bool]:
    original=sorted(original)
    original[0]=true
    return original
  first=f()
  first[1]=false
  second=f()
  return [first[1],second[1],original[0],original[1]]
