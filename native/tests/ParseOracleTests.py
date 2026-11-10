"""Check parsing against full-input exact rational arithmetic, not host float casts."""
import argparse
from fractions import Fraction
from pathlib import Path
import random
import re
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--driver',type=Path,required=True)
a=p.parse_args()
random.seed(0x5041525345)
sys.set_int_max_str_digits(20000)

def decimal(value):
    negative=value<0
    value=abs(value)
    denominator=value.denominator
    twos=fives=0
    while denominator%2==0:twos+=1;denominator//=2
    while denominator%5==0:fives+=1;denominator//=5
    assert denominator==1
    places=max(twos,fives)
    digits=str(value.numerator * 2**(places-twos) * 5**(places-fives))
    if places:
        digits=digits.zfill(places+1)
        digits=digits[:-places]+'.'+digits[-places:]
    return ('-' if negative else '')+digits

def rational(text):
    negative=text.startswith('-')
    unsigned=text.lstrip('+-')
    parts=re.split('[eE]',unsigned)
    exponent=int(parts[1]) if len(parts)==2 else 0
    significand=parts[0]
    if '.' in significand:
        whole,fraction=significand.split('.')
        exponent-=len(fraction)
        digits=whole+fraction
    else:digits=significand
    value=Fraction(int(digits),1)
    value=value*10**exponent if exponent>=0 else value/Fraction(10**-exponent)
    return -value if negative else value

def ieee(value,wide,negative_zero=False):
    sign=(1<<(63 if wide else 31)) if value<0 or (not value and negative_zero) else 0
    value=abs(value)
    if not value:return 0,sign
    numerator,denominator=value.numerator,value.denominator
    exponent=numerator.bit_length()-denominator.bit_length()
    if exponent>=0:
        if numerator<denominator<<exponent:exponent-=1
    elif numerator<<-exponent<denominator:exponent-=1
    precision,minimum,maximum=(53,-1022,1023) if wide else (24,-126,127)
    exponent=max(exponent,minimum)
    scale=exponent-precision+1
    if scale<0:numerator<<=-scale
    else:denominator<<=scale
    integer,remainder=divmod(numerator,denominator)
    integer+=remainder*2>denominator or (remainder*2==denominator and integer%2!=0)
    if not integer:return 2,0
    if integer==1<<precision:integer>>=1;exponent+=1
    if exponent>maximum:return 2,0
    hidden=1<<(precision-1)
    bits=integer if integer<hidden else ((exponent+(1023 if wide else 127))<<(precision-1))|(integer-hidden)
    return 0,sign|bits

def decode(bits,wide):
    precision,bias=(52,1023) if wide else (23,127)
    exponent=(bits>>precision)&((1<<(11 if wide else 8))-1)
    mantissa=bits&((1<<precision)-1)
    power=(exponent-bias if exponent else 1-bias)-precision
    integer=mantissa+((1<<precision) if exponent else 0)
    result=Fraction(integer*2**power) if power>=0 else Fraction(integer,2**-power)
    return -result if bits>>(63 if wide else 31) else result

cases=[]
def floating(text,wide):
    status,bits=ieee(rational(text),wide,text.startswith('-'))
    cases.append((10 if wide else 9,text,status,0,bits))

for wide in (False,True):
    width=64 if wide else 32
    max_exponent=(1<<(11 if wide else 8))-1
    mantissa_bits=52 if wide else 23
    # Exact binary values produce terminating decimals, including every sign,
    # exponent region, subnormals and huge decimal representations.
    for _ in range(1500):
        bits=random.getrandbits(width)
        if (bits>>mantissa_bits)&max_exponent==max_exponent:continue
        text=decimal(decode(bits,wide))
        floating(text,wide)
        status,expected=ieee(rational(text),wide)
        assert status==0 and expected==bits
    # Exact midpoints, and perturbations much smaller than the retained prefix.
    selected=[0,1,2,3,(1<<mantissa_bits)-1,1<<mantissa_bits,
              (127 if not wide else 1023)<<mantissa_bits,
              ((max_exponent-1)<<mantissa_bits)-1,
              ((max_exponent-1)<<mantissa_bits)]
    selected += [random.randrange(((max_exponent-1)<<mantissa_bits)) for _ in range(80)]
    for low in selected:
        middle=(decode(low,wide)+decode(low+1,wide))/2
        for delta in (Fraction(0),Fraction(1,10**1600),-Fraction(1,10**1600)):
            for sign in (1,-1):floating(decimal(sign*(middle+delta)),wide)
    # The finite/infinite rounding threshold is not the largest finite value.
    maximum=((max_exponent-1)<<mantissa_bits)|((1<<mantissa_bits)-1)
    largest=decode(maximum,wide)
    ulp=largest-decode(maximum-1,wide)
    for value in (largest,largest+ulp/4,largest+ulp/2,largest+ulp*3/4):
        floating(decimal(value),wide);floating(decimal(-value),wide)
    for _ in range(1000):
        digits=''.join(str(random.randrange(10)) for _ in range(random.randrange(1,120)))
        text=('-' if random.randrange(2) else '+')+digits+'e'+str(random.randrange(-500,501))
        floating(text,wide)
    for text in ('-0','+0.000','0e9999','-0e-9999','1e-5000','1e5000'):
        floating(text,wide)

for kind in range(1,9):
    signed=kind<=4
    width=8<<(kind-1 if signed else kind-5)
    minimum=-(1<<(width-1)) if signed else 0
    maximum=(1<<(width-1))-1 if signed else (1<<width)-1
    for value in [minimum-1,minimum,minimum+1,maximum-1,maximum,maximum+1]+[random.randrange(minimum,maximum+1) for _ in range(300)]:
        text=str(value)
        if not signed and value<0:cases.append((kind,text,1,0,0))
        elif minimum<=value<=maximum:cases.append((kind,text,0,0,value&((1<<width)-1)))
        else:cases.append((kind,text,2,0,0))

request=''.join(f'{kind}\t{text}\n' for kind,text,*_ in cases)
r=subprocess.run([str(a.driver.resolve()),'--batch'],input=request,capture_output=True,text=True,timeout=180)
assert r.returncode==0,(r.returncode,r.stderr)
lines=r.stdout.splitlines();assert len(lines)==len(cases),(len(lines),len(cases),r.stderr)
for index,(case,line) in enumerate(zip(cases,lines)):
    status,offset,bits=line.split()
    actual=(int(status),int(offset),int(bits,16))
    assert actual==tuple(case[2:]),(index,case,actual)
print(f'PASS: {len(cases)} independently rounded/range-checked scalar cases; exact rationals, IEEE midpoints, long tails and every integer width')
