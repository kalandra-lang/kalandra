import json,math,sys,time

def polynomial(x,d):
    if not d:
        v=float(x)
        return (v*0.125+0.25)*v-0.5
    return polynomial(x*2,d-1)+polynomial(x*2+1,d-1)
def branches(x,d):
    if not d:
        return float(x)*0.5 if x%3==0 else float(x)*(-0.25) if x%3==1 else float(x)+0.125
    return branches(x*2,d-1)+branches(x*2+1,d-1)
def gcd(a,b):
    return a if not b else gcd(b,a%b)
def euclid(x,d):
    return float(gcd(x*13+7,x*7+11)) if not d else euclid(x*2,d-1)+euclid(x*2+1,d-1)
if __name__=='__main__':
    kind,depth,iterations,rounds,seed=map(int,sys.argv[1:])
    assert 0<=kind<=2 and 0<=depth<=14 and 1<=seed<=31 and iterations>0 and rounds>0
    kernel=(polynomial,branches,euclid)[kind]
    for r in range(-2,rounds):
        checksum=0.0;start=time.perf_counter_ns()
        for _ in range(iterations):checksum+=kernel(seed,depth)
        elapsed=time.perf_counter_ns()-start
        print(json.dumps(dict(round=r,ns=elapsed,checksum=checksum)))
