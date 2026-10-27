"""Deterministic controls for warm batching, paired counts and legacy behavior."""
import math
from protocol import calibrate_batch

for cost in (.8, 7, 50, 1000, 1000000):
    def invoke(cell, count, rounds):
        return [dict(round=r, ns=20 + cost * count, checksum=0) for r in range(-2, rounds)]
    count, probes = calibrate_batch([['test']], invoke, 10, 20000000, True)
    assert count <= 20000000 and 20 + cost * count >= 8000000
    assert probes[-1]['iterations'] == count
    old_count, old_probes = calibrate_batch([['test']], invoke, 10, 20000, False)
    assert old_count == min(20000, math.ceil(10000000 / (20 + cost))) and len(old_probes) == 1


def pair(cell, count, rounds):
    cost = 1 if cell[0] == 'current' else 4
    return [dict(round=r, ns=20 + cost * count, checksum=0) for r in range(-2, rounds)]

count, probes = calibrate_batch([['baseline'], ['current']], pair, 10, 20000000, True)
assert count >= 7999980 and probes[-1]['iterations'] == probes[-2]['iterations'] == count
count, probes = calibrate_batch([['current']], pair, 10, 2, True)
assert count == 2
for invalid in (-1, float('inf'), float('nan')):
    try:
        calibrate_batch([['current']], lambda c, n, r: [dict(round=i, ns=invalid) for i in range(-2, r)], 10, 20, True)
    except AssertionError:
        pass
    else:
        raise AssertionError('invalid timer accepted')
def coarse(cell,count,rounds):
    return [dict(round=r,ns=0 if count<=32 else 2*count) for r in range(-2,rounds)]
count,_=calibrate_batch([['current']],coarse,10,20000000,True)
assert count>=4000000
print('adaptive convergence, faster-member pairing, cap, invalid timer and legacy controls passed')
