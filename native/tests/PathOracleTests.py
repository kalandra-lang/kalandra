"""Compare ordinary lexical paths with independent Python path libraries.

The corpus excludes incomplete/device namespace forms and bare-drive replacement,
whose deliberately explicit contracts have separate runtime tables. Windows
isAbsolute means fully qualified, as modelled by PureWindowsPath, rather than
older isabs implementations that treat a drive-relative root as absolute.
"""
import argparse
import ntpath
from pathlib import Path, PureWindowsPath
import posixpath
import random
import re
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--driver',type=Path,required=True)
a=p.parse_args()
random.seed(0x50415448)
cases=[]
names=['a','file.txt','.gitignore','.config.json','archive.tar.gz','file.','..hidden','...','.', '..','κόσμος','سلام.txt','space name','nul\0x']
for style,library,separators in [(0,posixpath,'/'),(1,ntpath,'/\\')]:
    prefixes=['','/','//','///'] if not style else ['', 'C:', 'C:\\', 'D:/', '\\', '/', '\\\\server\\share\\', '//server/share/']
    for _ in range(1800):
        components=[random.choice(names) for _ in range(random.randrange(0,7))]
        delim=random.choice(separators)
        text=random.choice(prefixes)+delim.join(components)
        if random.randrange(3)==0:text+=random.choice(separators)*random.randrange(1,4)
        if style and len(text)>=2 and text[0] in separators and text[1] in separators and not re.match(r'^[\\/]{2}[^\\/]+[\\/][^\\/]+(?:[\\/]|$)',text):
            continue # Incomplete/malformed network roots have explicit runtime tables.
        drive,tail=ntpath.splitdrive(text) if style else ('',text)
        stripped=tail.rstrip(separators)
        name=library.basename(stripped) if stripped else ''
        parent=(library.dirname(drive+stripped) or '.') if stripped else (text or '.')
        extension=library.splitext(name)[1]
        absolute=PureWindowsPath(text).is_absolute() if style else text.startswith('/')
        for action,expected in [(2,name),(3,parent),(4,extension),(5,absolute)]:
            cases.append((style,action,[text],int(expected) if action==5 else 0,'' if action==5 else expected))
    for _ in range(1400):
        parts=[]
        for _ in range(random.randrange(0,9)):
            component=random.choice(names+['','/new','C:child','D:child'] if style else names+['','/new','//host/share'])
            if component and random.randrange(5)==0:component+=random.choice(separators)
            parts.append(component)
        nonempty=[part for part in parts if part]
        expected=library.join(*nonempty) if nonempty else ''
        cases.append((style,1,parts,0,expected))

def encode(text):return text.encode('utf-8').hex() or '-'
request=''.join(f'{style} {action} {len(parts)} '+ ' '.join(map(encode,parts))+'\n' for style,action,parts,*_ in cases)
r=subprocess.run([str(a.driver.resolve()),'--batch'],input=request,text=True,capture_output=True,timeout=180)
assert not r.returncode,(r.returncode,r.stderr)
lines=r.stdout.splitlines();assert len(lines)==len(cases),(len(lines),len(cases),r.stderr)
for case,line in zip(cases,lines):
    flag,text=line.split()
    actual=int(flag),bytes.fromhex(text).decode('utf-8') if text!='-' else ''
    assert actual==tuple(case[3:]),(case,actual)
print(f'PASS: {len(cases)} independent POSIX/Windows component, extension, anchoring and join cases, including Unicode and NUL')
