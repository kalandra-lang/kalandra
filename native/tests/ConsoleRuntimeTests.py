"""Exercise console runtime using real redirected OS streams, including blocked I/O."""
import argparse
import os
import subprocess
import time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--driver',type=Path,required=True)
a=p.parse_args()
a.driver=a.driver.resolve()
def run(mode, data=b'', *args):
    result=subprocess.run([a.driver,mode,*map(str,args)],input=data,capture_output=True,timeout=15)
    assert result.returncode==0 and not result.stderr,(mode,args,result)
    return result.stdout if mode == 'write' else result.stdout.replace(b'\r\n',b'\n')

def line(data):
    return b'0:0:'+data.hex().encode()+b'\n'
end=b'1:0:\n'
assert run('write')=='α\0é\n'.encode()
assert run('read',b'',100,1)==end
assert run('read','α\0é\r\nsecond\nlast\r'.encode(),100,4)==line('α\0é'.encode())+line(b'second')+line(b'last\r')+end
assert run('read',b'\r\n\ntext\n',0,4)==line(b'')*2+b'4:0:\n'+end
assert run('read',b'toolong\nok\n',2,3)==b'4:0:\n'+line(b'ok')+end
assert run('read',b'\xff\nok\n',20,3)==b'3:0:\n'+line(b'ok')+end
assert run('read',b'a'*1023+b'\r\nnext\n',1023,3)==line(b'a'*1023)+line(b'next')+end
assert run('read',b'a'*300000+b'\n',300000,2)==line(b'a'*300000)+end
assert run('read',b'\xe2\x82',100,2)==b'3:0:\n'+end
assert run('precedence')==b''
for ordinal in range(1,16):
    output=run('read',b'a'*200+b'\n',500,1,ordinal)
    assert output==b'fault:2\n' or output==line(b'a'*200),(ordinal,output)

for mode in ('cancel-read','unbound-reader'):
    started=time.monotonic()
    read=subprocess.Popen([a.driver,mode],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        # A watchdog bounds this handshake even if cancellation regresses.
        import threading
        timer=threading.Timer(10,read.kill);timer.start()
        assert read.stdout.readline().rstrip(b'\r\n')==b'cancelled'
        output,error=read.communicate(b'resumed\n',timeout=10)
        assert read.returncode==0 and not error and output.replace(b'\r\n',b'\n')==line(b'resumed'),(mode,output,error)
    finally:
        timer.cancel()
        if read.poll() is None:read.kill();read.communicate()
    assert time.monotonic()-started<10

write=subprocess.Popen([a.driver,'cancel-write'],stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
try:
    timer=threading.Timer(10,write.kill);timer.start()
    assert write.stderr.readline().rstrip(b'\r\n')==b'cancelled'
    output,error=write.communicate(timeout=10)
    assert write.returncode==0 and not error and output.endswith(b'END\n'),(write.returncode,error)
    prefix=output[:-4]
    assert prefix and len(prefix)<8*1024*1024 and prefix==b'x'*len(prefix)
finally:
    timer.cancel()
    if write.poll() is None:write.kill();write.communicate()

read_fd,write_fd=os.pipe()
os.close(read_fd)
broken=subprocess.Popen([a.driver,'broken'],stdout=write_fd,stderr=subprocess.PIPE)
os.close(write_fd)
assert broken.wait(timeout=10)==0
assert broken.stderr.read().replace(b'\r\n',b'\n')==b'system-error\n'
print('PASS: Unicode/NUL, EOF, CRLF, line bounds, malformed input, allocation failures, blocked cancellation, subsequent calls and broken pipes')

for workers in (1,4):
    child=subprocess.Popen([a.driver,'scheduled',str(workers)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    timer=threading.Timer(10,child.kill);timer.start()
    try:
        assert child.wait(timeout=12)==0
        output,error=child.communicate(timeout=2)
        assert output.replace(b'\r\n',b'\n')==b'cancelled\n' and not error,(workers,output,error)
    finally:
        timer.cancel()
        if child.poll() is None:child.kill();child.communicate()
print('PASS: deterministic blocked reader yields its worker, sibling fault wins, frames and resource budget retire')
