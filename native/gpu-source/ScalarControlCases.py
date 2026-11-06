"""Independent-oracle cases shared by physical device-control qualification."""
from pathlib import Path

def cases():
    out = {
        'range-break-continue': ('fn work(n:int)->int:\n  total=0\n  for i in range(n):\n    if i%2==0:\n      continue\n    if i>100:\n      break\n    total=total+i\n  total\n', 'work(200)', 2500),
        'while-multiple-checkpoints': ('fn work(n:int)->int:\n  i=0\n  total=0\n  while i<n:\n    i=i+1\n    total=total+i\n  total\n', 'work(1000)', 500500),
        'nested-loops': ('fn work(n:int)->int:\n  total=0\n  for i in range(n):\n    for j in range(i):\n      if j==3:\n        break\n      if j==1:\n        continue\n      total=total+i+j\n  total\n', 'work(25)', sum(i+j for i in range(25) for j in range(min(i,3)) if j!=1)),
        'descending-range': ('fn work(n:int)->int:\n  total=0\n  for i in range(n,0,-3):\n    total=total+i\n  total\n', 'work(100)', sum(range(100,0,-3))),
        'zero-iterations': ('fn work(n:int)->int:\n  total=19\n  for i in range(n):\n    total=total+i\n  while false:\n    total=0\n  total\n', 'work(0)', 19),
        'loop-expression-break': ('fn work(n:int)->int:\n  result=for i in range(n):\n    if i==7:\n      break i*6\n  else:\n    9\n  result\n', 'work(100)', 42),
        'loop-expression-else': ('fn work(n:int)->int:\n  result=while n<0:\n    break 2\n  else:\n    42\n  result\n', 'work(0)', 42),
        'bool-and-conditional-state': ('fn work(n:int)->int:\n  i=0\n  enabled=true\n  total=0\n  while i<n:\n    if enabled:\n      total=total+2\n    else:\n      total=total+1\n    enabled=not enabled\n    i=i+1\n  total\n', 'work(101)', 152),
        'short-circuit': ('fn work(n:int)->int:\n  i=0\n  total=0\n  while i<n:\n    if false and 1/(n-n)==0:\n      total=99\n    if true or 1/(n-n)==0:\n      total=total+1\n    i=i+1\n  total\n', 'work(50)', 50),
        'helper-loop': ('fn inner(n:int)->int:\n  total=0\n  for i in range(n):\n    total=total+i\n  total\nfn work(n:int)->int:\n  total=0\n  for i in range(n):\n    total=total+inner(i)\n  total\n', 'work(12)', sum(sum(range(i)) for i in range(12))),
        'parameter-mutation': ('fn work(n:int)->int:\n  total=0\n  while n>0:\n    total=total+n\n    n=n-1\n  total\n', 'work(100)', 5050),
        'bool-helper': ('fn even(n:int)->Bool:\n  flag=true\n  for i in range(n):\n    flag=not flag\n  flag\nfn work(n:int)->int:\n  if even(n):\n    42\n  else:\n    13\n', 'work(100)', 42),
        'range-overflow-exhaustion': ('fn work(n:int)->int:\n  total=0\n  for i in range(n,9223372036854775807,2):\n    total=total+1\n  total\n', 'work(9223372036854775804)', 2),
    }
    for t in ('int8','int16','int32','int64','uint8','uint16','uint32','uint64','float32','float64'):
        out['numeric-'+t] = (f'fn work(n:int)->{t}:\n  total:{t}=0\n  for i in range(n):\n    total=total+{t}(1)\n  total\n', 'work(50)', 50)
        out['capture-'+t] = (f'fn work(n:{t})->{t}:\n  total:{t}=0\n  for i in range(10):\n    total=total+n\n  total\n', f'work({t}(3))', 30)
    out.update({
        'record-local-value': ('type Row:\n  value:int\n  enabled:Bool\nfn work(n:int)->int:\n  row=Row {value:n,enabled:true}\n  if row.enabled:\n    row.value+2\n  else:\n    0\n', 'work(40)', 42),
        'record-update-loop': ('type Row:\n  value:int\n  weight:int\nfn work(n:int)->int:\n  total=0\n  for i in range(n):\n    row=Row {value:i,weight:3}\n    updated=row with {value:row.value+2}\n    total=total+row.value+updated.value*updated.weight\n  total\n', 'work(100)', sum(i+(i+2)*3 for i in range(100))),
        'record-nested-helper': ('type Inner:\n  value:int\n  enabled:Bool\ntype Outer:\n  inner:Inner\n  weight:float32\nfn make(n:int)->Outer:\n  Outer {inner:Inner {value:n,enabled:true},weight:float32(2)}\nfn score(row:Outer)->int:\n  if row.inner.enabled:\n    row.inner.value*int(row.weight)\n  else:\n    0\nfn work(n:int)->int:\n  total=0\n  for i in range(n):\n    row=make(i)\n    total=total+score(row)\n  total\n', 'work(100)', 9900),
        'enum-local-match': ('enum Choice:\n  Empty\n  Some:\n    value:int\n    enabled:Bool\nfn score(choice:Choice)->int:\n  match choice:\n    Choice.Empty:1\n    Choice.Some {value,enabled}:\n      if enabled:value+7\n      else:0\nfn work(n:int)->int:\n  total=0\n  for i in range(n):\n    choice=if i%3==0:Choice.Some {value:i,enabled:true}\n    else:Choice.Empty\n    total=total+score(choice)\n  total\n', 'work(100)', sum(i+7 if i%3==0 else 1 for i in range(100))),
        'enum-nested-record': ('type Row:\n  value:int8\n  step:uint64\nenum Choice:\n  Missing\n  Present:\n    row:Row\nfn score(choice:Choice)->int:\n  match choice:\n    Choice.Missing:0\n    Choice.Present {row}:int(row.value)+int(row.step)\nfn work(n:int)->int:\n  total=0\n  for i in range(n):\n    choice=Choice.Present {row:Row {value:int8(i),step:uint64(2)}}\n    total=total+score(choice)\n  total\n', 'work(50)', 1325),
    })
    errors = (Path(__file__).resolve().parents[1] / 'metal-source' /
              'device-application-errors.k').read_text()
    out['application-error-owned-payload'] = (errors, 'work(0)', 18)
    out['application-error-recursive-propagation'] = (errors, 'work(40)', 18)
    out['application-error-success'] = (errors, 'work(-1)', 5)
    out['application-error-alternative-family'] = (errors, 'work(1)', 23)
    return out

def fault_cases():
    return {
        'zero-step-empty': ('fn work(n:int)->int:\n  total=0\n  for i in range(0,0,n):\n    total=total+i\n  total\n', 'work(0)'),
        'overflow-after-checkpoints': ('fn work(n:int)->int:\n  total=9223372036854775707\n  for i in range(n):\n    total=total+1\n  total\n', 'work(1000)'),
        'division-after-checkpoints': ('fn work(n:int)->int:\n  total=0\n  for i in range(n):\n    total=total+1/(n-i-1)\n  total\n', 'work(200)'),
    }
