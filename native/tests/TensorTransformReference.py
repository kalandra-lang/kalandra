"""Independent Decimal oracles for tensor transform backend qualification.

Every basic step is rounded to the source width. Reduction preserves the
specified adjacent-pair tree; softmax propagates its four-ULP exp interval.
"""
import decimal as dec
import math
import struct
dec.getcontext().prec=110
dec.getcontext().traps[dec.InvalidOperation]=False
dec.getcontext().traps[dec.DivisionByZero]=False
dec.getcontext().traps[dec.Overflow]=False
D=dec.Decimal
def to_bits(x,fmt,wire):
    try:return struct.unpack('<'+wire,struct.pack('<'+fmt,float(x)))[0]
    except OverflowError:return struct.unpack('<'+wire,struct.pack('<'+fmt,math.copysign(math.inf,float(x))))[0]
def from_bits(x,fmt,wire):return struct.unpack('<'+fmt,struct.pack('<'+wire,x))[0]
def reference(values,m,n,op,epsilon,fmt,wire,gpu):
    def q(x):return D.from_float(from_bits(to_bits(x,fmt,wire),fmt,wire))
    def tree(xs):
        while len(xs)>1:xs=[q(xs[i]+xs[i+1]) if i+1<len(xs) else xs[i] for i in range(0,len(xs),2)]
        return xs[0]
    if op==0:return [values[r*n+c] for c in range(n) for r in range(m)]
    result=[]
    for row in range(m):
        xs=[D.from_float(from_bits(v,fmt,wire)) for v in values[row*n:(row+1)*n]]
        if not xs:continue
        if op==2:
            denominator=q(q(q(tree([q(x*x) for x in xs])/D(n))+epsilon).sqrt())
            result.extend((q(x/denominator),q(x/denominator)) for x in xs)
            continue
        if any(x.is_nan() for x in xs) or all(x==D('-Infinity') for x in xs):
            result.extend((D('NaN'),D('NaN')) for x in xs);continue
        high=max(xs)
        exps=[D(int(x==high)) if high==D('Infinity') else q(q(x-high).exp()) for x in xs]
        low=[];upper=[]
        for x in exps:
            b=to_bits(x,fmt,wire)
            # Infinity indicator is exact; exp is bounded to four ULP.
            delta=0 if high==D('Infinity') else 4
            lo=max(0,b-delta);hi=b+delta
            if gpu and b<(1<<23):lo=0;hi=max(hi,1<<23)
            low.append(D.from_float(from_bits(lo,fmt,wire)));upper.append(D.from_float(from_bits(hi,fmt,wire)))
        minimum,maximum=tree(low),tree(upper)
        result.extend((q(lo/maximum),q(hi/minimum)) for lo,hi in zip(low,upper))
    return result
