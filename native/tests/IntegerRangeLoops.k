fn walk0(value:int,rounds:int)->float64:
  if rounds==0:float64(value)
  else:walk0((value*13+7)%1000003,rounds-1)
fn probe0(seed:int,rounds:int)->float64:
  walk0(seed,rounds)
fn walk1(value:int,rounds:int)->float64:
  if rounds==0:float64(value)
  else:walk1((value*13+7)%1000003,rounds-1)
fn probe1(seed:int,rounds:int)->float64:
  walk1(-1,rounds)
fn walk2(value:int,rounds:int)->float64:
  if rounds==0:float64(value)
  else:walk2((value*(-13)+7)%1000003,rounds-1)
fn probe2(seed:int,rounds:int)->float64:
  walk2(seed,rounds)
fn walk3(value:int,rounds:int)->float64:
  if rounds==0:float64(value)
  else:walk3((value*13-65536)%1000003,rounds-1)
fn probe3(seed:int,rounds:int)->float64:
  walk3(seed,rounds)
fn walk4(value:int,rounds:int)->float64:
  if rounds==0:float64(value)
  else:walk4((value*13+7)%1000003,rounds-1)
fn probe4(seed:int,rounds:int)->float64:
  walk4(seed*65536,rounds)
fn walk5(value:int,rounds:int)->float64:
  if rounds==0:float64(value)
  else:walk5((value*13+7)%(-1000003),rounds-1)
fn probe5(seed:int,rounds:int)->float64:
  walk5(seed,rounds)
fn walk6(value:int,rounds:int)->float64:
  if rounds==0:float64(value)
  else:walk6((value*9223372036854775807+7)%1000003,rounds-1)
fn probe6(seed:int,rounds:int)->float64:
  walk6(seed,rounds)
fn main()->float64:
  probe0(17,16)
