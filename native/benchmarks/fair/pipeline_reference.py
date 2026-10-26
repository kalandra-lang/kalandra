"""Independent fixed-point tree and high-precision API references for ReLU pipelines."""
from decimal import Decimal, localcontext
import functools
import math
from library_reference import inputs


def round32_integer(value):
    shift=max(0,value.bit_length()-24)
    if not shift:return value
    q,r=divmod(value,1<<shift)
    half=1<<(shift-1)
    return (q+int(r>half or r==half and q&1))<<shift


@functools.cache
def exact(n):
    return exact_rect(n,n,n)


@functools.cache
def exact_rect(rows,inner,columns):
    # All products are integer/64; bias+scale produces integer/256.
    # Round every adjacent pair in integer arithmetic, including large sums.
    phase=[[max(0,sum(((a+k)%17-8)*((b+k*columns)%13-6) for k in range(inner))+8)
            for b in range(13)] for a in range(17)]
    values=[phase[(r*inner)%17][c%13] for r in range(rows) for c in range(columns)]
    while len(values)>1:
        values=[round32_integer(values[i]+values[i+1]) for i in range(0,len(values)-1,2)]+(values[-1:] if len(values)&1 else [])
    return values[0]/256


def assess(n,pattern,actual,cells):
    a,b=inputs(n,2,'float32',pattern)
    with localcontext() as context:
        context.prec=100
        a=list(map(Decimal.from_float,a));b=list(map(Decimal.from_float,b))
        u=Decimal(2)**-24
        gamma=lambda count:count*u/(1-count*u)
        expected=[];bounds=[]
        for r in range(n):
            for c in range(n):
                terms=[a[r*n+k]*b[k*n+c] for k in range(n)]
                expected.append(max(Decimal(0),(sum(terms)+Decimal(1)/8)/4))
                # ReLU is 1-Lipschitz; propagate GEMM plus two post-operation errors.
                bounds.append(gamma(2*n+2)*(sum(map(abs,terms))+Decimal(1)/8)/4)
        assert len(cells)==len(expected)
        error=Decimal(0)
        for got,want,bound in zip(cells,expected,bounds):
            delta=abs(Decimal.from_float(float(got))-want)
            assert math.isfinite(got) and delta<=bound,(n,pattern,got,str(want),str(delta),str(bound))
            error=max(error,delta)
        scalar_bound=sum(bounds)+gamma(2*n*n)*(sum(expected)+sum(bounds))
        scalar_error=abs(Decimal.from_float(float(actual))-sum(expected))
        assert math.isfinite(actual) and scalar_error<=scalar_bound
        return dict(cells=len(cells),max_cell_error=float(error),scalar_error=float(scalar_error),scalar_bound=float(scalar_bound))
