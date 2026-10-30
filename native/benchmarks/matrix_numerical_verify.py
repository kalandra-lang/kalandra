"""Correctness-only exact-integer oracle for frozen SIMD matrix prototypes.

All finite float32 products are represented exactly as integers scaled by 2^-298.
This oracle performs no floating-point multiplication or addition of the inputs.
It records, rather than hides, departures from the existing fixed-tree policy.
"""
import argparse
import array
import json
import math
import os
from pathlib import Path
import random
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent/'fair'))
import protocol as p


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def integer(word):
    """Exact finite float32 times 2^149, including subnormals."""
    exponent = (word >> 23) & 255
    assert exponent != 255
    value = word & 0x7fffff
    if exponent:
        value = (value | 0x800000) << (exponent-1)
    return -value if word >> 31 else value


def category(word):
    if word & 0x7f800000 == 0x7f800000:
        return 'nan' if word & 0x7fffff else '-inf' if word >> 31 else '+inf'
    if word & 0x7fffffff == 0:
        return '-zero' if word >> 31 else '+zero'
    return 'subnormal' if word & 0x7f800000 == 0 else 'normal'


def flush(word):
    return word & 0x80000000 if category(word)=='subnormal' else word


def rounded(value, negative_zero=False):
    """Round exact integer*2^-298 to nearest-even float32, then hardware FTZ."""
    sign=0x80000000 if value<0 or (value==0 and negative_zero) else 0
    value=abs(value)
    if not value:
        return sign
    shift=max(149,value.bit_length()-24)
    mantissa,remainder=divmod(value,1<<shift)
    halfway=1<<(shift-1)
    mantissa += remainder>halfway or (remainder==halfway and mantissa%2==1)
    if mantissa==1<<24:
        mantissa>>=1;shift+=1
    if mantissa<1<<23:
        return sign  # Subnormal arithmetic results flush to signed zero.
    exponent=shift-148
    if exponent>=255:
        return sign|0x7f800000
    return sign|(exponent<<23)|(mantissa&0x7fffff)


def operation(a,b,c=None):
    """Independent separately rounded product or fused multiply-add model."""
    a,b=flush(a),flush(b)
    if c is not None:
        c=flush(c)
    operands=(a,b) if c is None else (a,b,c)
    if any(category(x)=='nan' for x in operands):
        return 0x7fc00000
    sign=(a^b)&0x80000000
    if category(a).endswith('inf') or category(b).endswith('inf'):
        if a&0x7fffffff==0 or b&0x7fffffff==0:
            return 0x7fc00000
        product=sign|0x7f800000
        if c is not None and category(c).endswith('inf') and (c^product)>>31:
            return 0x7fc00000
        return product
    if c is not None and category(c).endswith('inf'):
        return c
    product=integer(a)*integer(b)
    if c is None:
        return rounded(product,bool(sign))
    return rounded(product+(integer(c)<<149),
                   product==0 and integer(c)==0 and bool(sign&c&0x80000000))


# Check finite decoding/rounding against an independent decimal representation.
from decimal import Decimal, localcontext
with localcontext() as context:
    context.prec=400
    for word in (0,0x80000000,1,0x80000001,0x007fffff,0x00800000,
                 0x3f800001,0x7f7fffff,0xff7fffff):
        value=struct.unpack('<f',struct.pack('<I',word))[0]
        assert Decimal(integer(word)) == Decimal(value)*(Decimal(2)**149)
        assert rounded(integer(word)<<149,bool(word>>31))==flush(word)
assert rounded((integer(bits(1))<<149)+(1<<274))==bits(1)  # halfway ties-even
assert rounded((integer(bits(1))<<149)+(1<<274)+1)==bits(1+2**-23)


ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build', type=Path, required=True)
ap.add_argument('--output', type=Path, required=True)
a = ap.parse_args()
b = a.build.resolve()
assert not b.exists() and not a.output.exists()
b.mkdir(parents=True)
driver = Path(__file__).with_name('MatrixNumericalProbe.mm')
naive = p.ROOT/'benchmarks/.build/gpu-matrix-simd-v3-2026-10-30/study.metallib'
blocked = p.ROOT/'benchmarks/.build/gpu-matrix-simd-block32-2026-10-30/study.metallib'
for build in (naive.parent, blocked.parent):
    manifest = json.loads((build/'manifest.json').read_text())
    for name, digest in manifest['artifacts'].items():
        assert p.sha(p.ROOT/name) == digest
p.run([p.CLANG, '-std=c++23', '-O2', '-fobjc-arc', '-fblocks', driver,
       '-framework', 'Foundation', '-framework', 'Metal', '-o', b/'probe'])
rng = random.Random(20261030)
cases = []


def case(name, left, right, *, k=512, mode='bounded', expected=None, m=32, n=32):
    av = [left(r, i) for r in range(m) for i in range(k)]
    bv = [right(i, c) for i in range(k) for c in range(n)]
    cases.append(dict(name=name, shape=[m, k, n], mode=mode, expected=expected, a=av, b=bv))


one = bits(1)
case('ones', lambda r,i:one, lambda i,c:one)
case('negative-zero', lambda r,i:0x80000000, lambda i,c:one, mode='zero')
case('positive-zero', lambda r,i:0, lambda i,c:one, mode='zero')
for k in (512, 576, 1024, 2048, 4096):
    case(f'cancellation-{k}', lambda r,i:bits((1e20,1,-1e20,1)[i%4]),
         lambda i,c:one, k=k)
case('small-residual', lambda r,i:bits((1,2**-20,-1,2**-20)[i%4]), lambda i,c:one)
case('fused-rounding-witness', lambda r,i:bits((-4096,4097)[i%2]),
     lambda i,c:bits((4098,4097)[i%2]))
case('input-mantissa-witness', lambda r,i:bits((1+2**-20,-1)[i%2]), lambda i,c:one)
for seed in range(3):
    # Every matrix cell is independently randomized, not a repeated-row oracle.
    av = [rng.getrandbits(1)<<31 | rng.randrange(107,148)<<23 | rng.getrandbits(23)
          for _ in range(64*576)]
    bv = [rng.getrandbits(1)<<31 | rng.randrange(107,148)<<23 | rng.getrandbits(23)
          for _ in range(576*64)]
    case(f'random-exponents-{seed}', lambda r,i:av[r*576+i],
         lambda i,c:bv[i*64+c], k=576, m=64, n=64)
case('identity-selection', lambda r,i:bits((r*17+i%19-12)/32),
     lambda i,c:one if i==c else 0)
case('minimum-normal-preserved', lambda r,i:0x00800000, lambda i,c:one)
case('minimum-normal-scaled', lambda r,i:0x00800000, lambda i,c:bits(2**126))
case('maximum-finite-preserved', lambda r,i:0x7f7fffff if i==0 else 0,
     lambda i,c:one)
# Underflow is diagnosed against exact arithmetic without pretending that the
# current hardware policy promises gradual underflow (it already permits FTZ).
case('subnormal-input-scaled', lambda r,i:1, lambda i,c:bits(2**126), mode='underflow')
case('subnormal-products', lambda r,i:0x00800000, lambda i,c:bits(0.5), mode='underflow')
case('tiny-products', lambda r,i:bits(2**-100), lambda i,c:bits(2**-100), mode='underflow')
case('positive-infinity', lambda r,i:0x7f800000, lambda i,c:one, mode='special', expected='+inf')
case('negative-infinity', lambda r,i:0xff800000, lambda i,c:one, mode='special', expected='-inf')
case('opposite-infinities', lambda r,i:(0x7f800000,0xff800000)[i%2],
     lambda i,c:one, mode='special', expected='nan')
case('zero-times-infinity', lambda r,i:0, lambda i,c:0x7f800000, mode='special', expected='nan')
case('quiet-nan', lambda r,i:0x7fc12345, lambda i,c:one, mode='special', expected='nan')
case('signaling-nan', lambda r,i:0x7f812345, lambda i,c:one, mode='special', expected='nan')
case('finite-overflow', lambda r,i:0x7f7fffff, lambda i,c:bits(2), mode='special', expected='+inf')
case('intermediate-overflow-cancellation', lambda r,i:0x7f7fffff,
     lambda i,c:bits((2,-2)[i%2]), mode='overflow-diagnostic')

records=[]
for item in cases:
    name=item['name']; m,k,n=item['shape']
    inputfile=b/(name+'.input')
    inputfile.write_bytes(struct.pack('<'+'I'*(len(item['a'])+len(item['b'])), *item['a'], *item['b']))
    exact=[]; absolute=[]; tree_model=[]; fused_model=[]
    model_cache={}
    cols=[tuple(item['b'][i*n+c] for i in range(k)) for c in range(n)]
    for r in range(m):
        row=tuple(item['a'][r*k:(r+1)*k])
        for col in cols:
            key=(row,col)
            if key not in model_cache:
                tree=[operation(x,y) for x,y in zip(row,col)]
                while len(tree)>1:
                    tree=[operation(tree[i],bits(1),tree[i+1])
                          if i+1<len(tree) else tree[i] for i in range(0,len(tree),2)]
                acc=0
                for x,y in zip(row,col):
                    acc=operation(x,y,acc)
                model_cache[key]=(tree[0],acc)
            tree,fused=model_cache[key];tree_model.append(tree);fused_model.append(fused)
    if item['mode'] != 'special':
        av=[integer(x) for x in item['a']]; bv=[integer(x) for x in item['b']]
        cache={}
        columns=[tuple(bv[i*n+c] for i in range(k)) for c in range(n)]
        for r in range(m):
            row=tuple(av[r*k:(r+1)*k])
            for col in columns:
                key=(row,col)
                if key not in cache:
                    products=[x*y for x,y in zip(row,col)]
                    cache[key]=(sum(products),sum(map(abs,products)))
                value,total=cache[key];exact.append(value);absolute.append(total)
    outputs={}
    for variant,lib in [('baseline',naive),('naive',naive),('blocked',blocked)]:
        out=b/(name+'-'+variant+'.output')
        result=json.loads(p.run([b/'probe',lib,variant,m,k,n,inputfile,out,3],
                          env=dict(os.environ,MTL_DEBUG_LAYER='1',MTL_SHADER_VALIDATION='1'),timeout=180))
        assert result==dict(rounds=3,cells=m*n,guards=True,inputs_unchanged=True)
        words=list(struct.unpack('<'+'I'*(m*n*3),out.read_bytes()))
        assert words[:m*n]==words[m*n:2*m*n]==words[2*m*n:]
        words=words[:m*n];outputs[variant]=words
        counts={cat:sum(category(x)==cat for x in words) for cat in set(map(category,words))}
        record=dict(name=name,shape=item['shape'],mode=item['mode'],variant=variant,
                    classes=counts,repeatable=True,guards=True,inputs_unchanged=True,
                    input_sha256=p.sha(inputfile),output_sha256=p.sha(out),output=p.key(out),
                    first_bits=f'{words[0]:08x}',different_from_baseline=sum(x!=y for x,y in zip(words,outputs['baseline'])))
        equal=lambda x,y:x==y or category(x)==category(y)=='nan'
        record['tree_model_mismatches']=sum(not equal(x,y) for x,y in zip(words,tree_model))
        record['sequential_fma_model_mismatches']=sum(not equal(x,y) for x,y in zip(words,fused_model))
        if variant=='baseline':
            assert not record['tree_model_mismatches'],record
        if item['expected']:
            assert set(counts)=={item['expected']}, record
            record['expected_class_pass']=True
        if exact:
            finite=[i for i,x in enumerate(words) if category(x) not in ('nan','+inf','-inf')]
            errors=[abs((integer(words[i])<<149)-exact[i]) for i in finite]
            # Exact rational comparison to gamma(K)*sumabs; no floating oracle.
            failures=sum(error*((1<<24)-k)>k*absolute[i] for i,error in zip(finite,errors))
            ratios=[error*((1<<24)-k)/(k*absolute[i]) for i,error in zip(finite,errors) if absolute[i]]
            relative=[error/abs(exact[i]) for i,error in zip(finite,errors) if exact[i]]
            record.update(nonfinite_cells=m*n-len(finite),bound_failures=failures,
                          max_error_bound_ratio=max(ratios,default=0),
                          max_relative_error=max(relative,default=0),
                          max_absolute_error=math.ldexp(float(max(errors,default=0)),-298),
                          exact_cells=sum(error==0 for error in errors),
                          exact_first=math.ldexp(float(exact[0]),-298))
            if item['mode']=='bounded':
                assert not record['nonfinite_cells'] and not failures,record
                record['finite_accuracy_pass']=True
        records.append(record)
    print(name,[(x['variant'],x['classes'],x.get('max_relative_error')) for x in records[-3:]],flush=True)

data=dict(contract=__doc__,production_change=False,performance_acceptance=False,
          cases=len(cases),records=records,
          oracle='Exact integer products/sums at scale 2^-298; error comparison uses exact integer gamma(K) bound. No double-precision oracle rounding.',
          limitations='Aligned finite prototype domain only; no new API contract, shape tails, runtime cancellation/resource integration or whole-model acceptance.')
p.evidence(a.output,data,[Path(__file__),driver,Path(p.__file__)],[b/'probe',naive,blocked],p.CLANG)
