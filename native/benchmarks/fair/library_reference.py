"""Exact input references and conservative, documented floating error bounds."""
import functools,math,struct
from decimal import Decimal,localcontext
from fractions import Fraction

def rounded(value,dtype):
    return struct.unpack('f',struct.pack('f',value))[0] if dtype=='float32' else float(value)
def inputs(n,kind,dtype,pattern):
    count=n*n if kind==2 else n
    if not pattern:return ([rounded((i%17-8)/8,dtype) for i in range(count)], [rounded((i%13-6)/8,dtype) for i in range(count)])
    return ([rounded(((i*37)%101-50)/37,dtype) for i in range(count)], [rounded(((i*43)%101-50)/29,dtype) for i in range(count)])
@functools.cache
def exact(kind,n):
    if kind==0:return sum((i%17-8)*(i%13-6) for i in range(n))/64
    if kind==1:return sum(3*(i%17-8)+(i%13-6) for i in range(n))/8
    # Exact integer distributivity, O(n^2). No floating library used by oracle.
    return sum(sum((r*n+k)%17-8 for r in range(n))*sum((k*n+c)%13-6 for c in range(n)) for k in range(n))/64

def assess(kind,n,dtype,pattern,actual,cells=None):
    a,b=inputs(n,kind,dtype,pattern)
    with localcontext() as context:
        context.prec=100
        a=list(map(Decimal.from_float,a));b=list(map(Decimal.from_float,b))
        u=Decimal(2)**(-24 if dtype=='float32' else -53)
        gamma=lambda ops:(ops*u)/(1-ops*u)
        expected=[];bounds=[]
        if kind==0:
            terms=[x*y for x,y in zip(a,b)];target=sum(terms);bound=gamma(2*n)*sum(map(abs,terms))
        elif kind==1:
            expected=[3*x+y for x,y in zip(a,b)]
            bounds=[gamma(2)*(3*abs(x)+abs(y)) for x,y in zip(a,b)]
            target=sum(expected);bound=sum(bounds)+gamma(2*n)*(sum(map(abs,expected))+sum(bounds))
        else:
            for r in range(n):
                for c in range(n):
                    terms=[a[r*n+k]*b[k*n+c] for k in range(n)]
                    expected.append(sum(terms));bounds.append(gamma(2*n)*sum(map(abs,terms)))
            target=sum(expected);bound=sum(bounds)+gamma(2*n*n)*(sum(map(abs,expected))+sum(bounds))
        error=abs(Decimal.from_float(float(actual))-target)
        assert math.isfinite(actual) and error<=bound,(kind,n,dtype,pattern,actual,str(target),str(error),str(bound))
        largest=float(error)
        if cells is not None and kind!=0:
            assert len(cells)==len(expected)
            for i,(value,want,tolerance) in enumerate(zip(cells,expected,bounds)):
                delta=abs(Decimal.from_float(float(value))-want)
                assert math.isfinite(value) and delta<=tolerance,(kind,n,dtype,pattern,i,value,str(want),str(delta),str(tolerance))
                largest=max(largest,float(delta))
        return dict(max_absolute_error=largest,scalar_error_bound=float(bound),cells_checked=0 if cells is None else len(cells))
