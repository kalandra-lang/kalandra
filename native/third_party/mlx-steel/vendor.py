"""Audit the exact MIT-only embedded dependency closure; no network access.

With --upstream PATH also compare every original file against the pinned checkout.
No NAX implementation, framework runtime, CUTLASS or CUDA DSL is imported.
"""
import argparse,hashlib,json,posixpath,re,subprocess
from pathlib import Path,PurePosixPath
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--upstream',type=Path)
a=ap.parse_args();root=Path(__file__).resolve().parent
manifest=json.loads((root/'UPSTREAM.json').read_text(encoding='utf-8'))
sha=lambda b:hashlib.sha256(b).hexdigest()
license=(root/'LICENSE').read_bytes();assert sha(license)==manifest['license_sha256']
assert license.startswith(b'MIT License\n')
text=(root/'SteelUpstream.h').read_text(encoding='utf-8')
assert sha(text.encode())==manifest['embedded_sha256']
sections=re.findall(r'// BEGIN upstream ([^\n]+)\n(.*?)\n// END upstream \1\n',text,re.S)
assert len(sections)==15 and {name for name,_ in sections}==set(manifest['files'])
# Account for the whole embedding, not only named regions: no extra shader
# source may hide outside the audited bodies, and both notices must be complete.
expected=('// Generated from pinned MIT-licensed MLX headers. See LICENSE and UPSTREAM.json.\n'
          '#pragma once\ninline constexpr const char *kalandraSteelUpstream = R"MLXSTEEL(\n/*\n'
          +license.decode('utf-8')+'*/\n')
expected+=''.join('// BEGIN upstream '+name+'\n'+body+'\n// END upstream '+name+'\n\n' for name,body in sections)
expected+=')MLXSTEEL";\n\ninline constexpr const char *kalandraSteelLicense = R"MLXLICENSE('+license.decode('utf-8')+')MLXLICENSE";\n'
assert text==expected, 'Unaccounted source or missing license in embedded header'
for name,body in sections:
 assert name.startswith('mlx/backend/metal/kernels/') and name.endswith('.h')
 assert not any(word in name.lower() for word in ('nax','cutlass','cuda','eula'))
 assert sha(body.encode())==manifest['flattened_sha256'][name],name
 assert not re.search(r'^\s*#include "',body,re.M),name
if a.upstream:
 revision=subprocess.check_output(['git','-C',str(a.upstream),'rev-parse','HEAD'],text=True).strip()
 assert revision==manifest['commit']
 assert (a.upstream/'LICENSE').read_bytes()==license
 for name,body in sections:
  original=(a.upstream/name).read_bytes();assert sha(original)==manifest['files'][name],name
  for dependency in re.findall(r'^\s*#include "([^"\n]+)"',original.decode('utf-8'),re.M):
   resolved=dependency if dependency in manifest['files'] else posixpath.normpath(str(PurePosixPath(name).parent/dependency))
   assert resolved in manifest['files'], (name,'unlisted dependency',dependency)
  flattened=''.join(line for line in original.decode().splitlines(True)
                    if not re.match(r'\s*#include "',line) and not line.strip().startswith('#pragma once'))
  assert flattened==body,name
print('MLX Steel MIT audit: 15 pinned headers, unchanged source bodies, complete embedded closure and retained license.')
