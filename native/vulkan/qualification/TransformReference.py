"""Independent float32 interval oracle for the Vulkan hardware transform profile.

Source: https://docs.vulkan.org/spec/latest/appendices/spirvenv.html
Basic operations use Vulkan implementation-defined rounding, without
contraction or reassociation. Both nearest enclosing float32 values are
propagated; explicit RTE triggered an NVIDIA driver compiler trap in testing.
FDiv has 2.5 ULP error for normal denominators in [2^-126,2^126]; exp
has (3+2*abs(x)) ULP error; sqrt inherits 1/inversesqrt, whose error is
2 ULP. Bounds are propagated through the original adjacent-pair tree.
These are specification bounds, not tolerances fitted to measured outputs.
The CPU/Metal oracle remains unchanged. Special-value rows additionally use
its existing explicit NaN/infinity contract; transpose is always bit-exact.
"""
import math
from TensorTransformReference import D, to_bits, from_bits, reference as metal_reference

MIN_NORMAL=D(2)**-126
MIN_SUBNORMAL=D(2)**-149
MAX_DIVISOR=D(2)**126

def q(x):
    return D.from_float(from_bits(to_bits(x,'f','I'),'f','I'))

def ulp(x):
    x=abs(x)
    if x==0 or x<MIN_NORMAL:return MIN_SUBNORMAL
    assert x.is_finite()
    exponent=math.frexp(float(x))[1]-1
    # At an exactly representable power of two, the specification's minimum
    # bracketing spacing is on the lower side of that binade boundary.
    if x==D(2)**exponent:exponent-=1
    return D(2)**max(-149,min(104,exponent-23))

def representable(value,direction):
    r=q(value)
    if r.is_finite() and ((direction<0 and r>value) or (direction>0 and r<value)):
        bits=to_bits(r,'f','I')
        if r==0:bits=0x80000001 if direction<0 else 1
        else:bits+=direction if r>0 else -direction
        r=D.from_float(from_bits(bits,'f','I'))
    return r

def rounded(lo,hi):
    # "Correctly rounded with implementation-defined rounding mode" permits
    # either nearest enclosing float, not an arbitrary error at every step.
    lo,hi=representable(lo,-1),representable(hi,1)
    if 0<lo<MIN_NORMAL:lo=D(0)
    if -MIN_NORMAL<hi<0:hi=D(0)
    return lo,hi

def envelope(x,error):
    if not x.is_finite():return x,x
    # Keep representable results inside the specified error interval. A bound
    # itself is not a new arithmetic operation with a free rounding allowance.
    return rounded(representable(x-error,1),representable(x+error,-1))

def add(a,b):return rounded(a[0]+b[0],a[1]+b[1])

def tree(values):
    while len(values)>1:
        values=[add(values[i],values[i+1]) if i+1<len(values) else values[i]
                for i in range(0,len(values),2)]
    return values[0]

def divide(a,b):
    assert MIN_NORMAL<=b[0]<=b[1]<=MAX_DIVISOR, ('oracle divisor outside guaranteed range',b)
    candidates=[]
    for x in a:
        for y in b:
            exact=x/y
            candidates.extend(envelope(exact,D('2.5')*ulp(exact)))
    return min(candidates),max(candidates)

def sqrt(a):
    assert 0<a[0]<=a[1] and a[1].is_finite(),a
    # Inherited precision means apply the inverse-sqrt error first, then the
    # reciprocal division error, rather than guessing a fixed sqrt ULP count.
    inverses=[]
    for x in a:
        exact=1/x.sqrt()
        inverses.extend(envelope(exact,2*ulp(exact)))
    return divide((D(1),D(1)),(min(inverses),max(inverses)))

def exp(a):
    results=[]
    for x in a:
        exact=x.exp()
        results.extend(envelope(exact,(3+2*abs(x))*ulp(exact)))
    return max(D(0),min(results)),max(results)

def reference(values,m,n,op,epsilon,fmt,wire,gpu=True):
    if op==0:return metal_reference(values,m,n,op,epsilon,fmt,wire,gpu)
    assert fmt=='f' and wire=='I'
    result=[]
    for row in range(m):
        bits=values[row*n:(row+1)*n]
        xs=[D.from_float(from_bits(v,fmt,wire)) for v in bits]
        if not xs:continue
        if op==1 and any(x==D('Infinity') for x in xs) and not any(x.is_nan() for x in xs):
            count=D(sum(x==D('Infinity') for x in xs))
            result.extend(divide((D(1),D(1)),(count,count)) if x==D('Infinity') else (D(0),D(0)) for x in xs)
            continue
        # Keep the stronger existing explicit special-value checks. The
        # generated shader handles NaN/+Inf softmax before native exp.
        if any(not x.is_finite() for x in xs) or any(abs(x)>D(2)**60 for x in xs):
            result.extend(metal_reference(bits,1,n,op,epsilon,fmt,wire,gpu));continue
        operands=[rounded(x,x) for x in xs]
        if op==2:
            squares=[]
            for lo,hi in operands:
                candidates=[lo*lo,hi*hi]
                squares.append(rounded(D(0) if lo<=0<=hi else min(candidates),max(candidates)))
            mean=divide(tree(squares),(D(n),D(n)))
            denominator=sqrt(add(mean,rounded(epsilon,epsilon)))
            result.extend(divide(x,denominator) for x in operands)
        else:
            high=max(xs)
            exps=[exp(rounded(x[0]-high,x[1]-high)) for x in operands]
            denominator=tree(exps)
            result.extend(divide(x,denominator) for x in exps)
    return result
