fn arrayRounds(values:[int],rounds:int)->float64:
  if rounds == 0:
    float64(values[0]+values[1]+values[2]+values[3]+values[4]+values[5]+values[6]+values[7])
  else:
    next=map(values,fn(value:int): (value*13+7)%1000003)
    arrayRounds(next,rounds-1)

fn arrays(seed:int,rounds:int)->float64:
  arrayRounds([seed,seed+1,seed+2,seed+3,seed+4,seed+5,seed+6,seed+7],rounds)

fn makeTransform(offset:int)->fn(int)->int:
  fn(value): (value*13+offset)%1000003

fn applyRounds(transform:fn(int)->int,value:int,rounds:int)->float64:
  if rounds == 0:
    float64(value)
  else:
    applyRounds(transform,transform(value),rounds-1)

fn closure(seed:int,rounds:int)->float64:
  applyRounds(makeTransform(seed+7),seed,rounds)

fn main()->float64:
  arrays(17,16)
