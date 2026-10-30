"""Unfinished-code queries retain real diagnostics, source bytes and live imports."""
from pathlib import Path
import sys
import tempfile
from ProtocolClient import Client, publications

server, root = sys.argv[1], Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)
def point(text, offset):
    prefix = text[:offset]
    return prefix.count('\n'), len(prefix.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2

with tempfile.TemporaryDirectory(dir=root) as temporary:
    folder = Path(temporary).resolve()
    path = folder / 'unfinished é.k'
    helper = folder / 'helper.k'
    helper_text = 'export type Box:\n  value:int\nexport fn add(left:int,right:int)->int:\n  left\n'
    helper.write_text(helper_text)
    client = Client(server)
    client.init(signature_offsets=True)
    base = ('from "./helper.k" import add, Box\n'
            'fn consume(values:[int])->int:\n  length(values)\n'
            'fn main():\n  greeting="😀"\n  value=2\n  values=[1,2]\n')
    version = 0
    def install(marked):
        global version
        offset = marked.rindex('|')
        text = marked[:offset] + marked[offset+1:]
        version += 1
        if version == 1: client.open(path, text, version)
        else: client.change(path, text, version)
        report = publications(client.barrier())[path.as_uri()]
        assert report['version'] == version and report['diagnostics']
        assert all(d['code'] != 'E001' and '__kalandra' not in d['message'] for d in report['diagnostics'])
        return text, offset, point(text, offset), report
    for body in ('values[va|', '[va|,', 'if true and va|', 'if true:\n    va|',
                 'consume([va|', 'value + va|]', 'add(1,2+va|Suffix'):
        text, offset, pos, report = install(base + '  ' + body + '\n')
        response = client.completion(path, *pos)['result']
        candidates = {item['label']:item for item in response['items']}
        assert 'value' in candidates, (body, response)
        item = candidates['value']
        assert item['textEdit']['newText'] == 'value'
        assert not publications(client.barrier()), 'query changed diagnostics'
        assert client.hover(path, *pos)['result'] is None
    for body in ('add(1,2+va|Suffix)', 'add(1,2+va|Suffix', 'add(1,values[va|',
                 'add(1,if true: va|', 'add(1,2+|', 'add(1,va|]'):
        text, offset, pos, report = install(base + '  ' + body + '\n')
        hint = client.signature(path, *pos)['result']
        assert hint and hint['activeParameter'] == 1, (body,hint)
        assert 'right: int64' in hint['signatures'][0]['label']
        assert not publications(client.barrier()), 'signature changed diagnostics'
    for body in ('add(missing,va|', 'missing(1,va|', 'add(1,missing+va|'):
        text, offset, pos, report = install(base + '  ' + body + '\n')
        assert client.signature(path, *pos)['result'] is None, body
    # A separate broken declaration cannot swallow the queried body.
    text, offset, pos, _ = install('fn broken():\n  add(1,\n' + base + '  va|\n')
    assert any(i['label']=='value' for i in client.completion(path,*pos)['result']['items'])
    # Current imported fields and parameters participate; disk stays unchanged.
    client.open(helper, helper_text, 1)
    client.barrier()
    text, offset, pos, _ = install('from "./helper.k" import Box\nfn inspect(box:Box):\n  box.va|\nfn main():\n  0\n')
    assert any(i['label']=='value' for i in client.completion(path,*pos)['result']['items'])
    updated = helper_text.replace('right:int','right:String').replace('value:int','values:int')
    client.change(helper, updated, 2)
    client.barrier()
    assert any(i['label']=='values' for i in client.completion(path,*pos)['result']['items'])
    text, offset, pos, _ = install(base + '  add(1,va|\n')
    hint=client.signature(path,*pos)['result']
    assert hint and 'right: String' in hint['signatures'][0]['label'],hint
    assert helper.read_text()==helper_text and not path.exists()
    # Finishing the actual document clears errors through normal strict checking.
    client.change(path, 'fn main()->int:\n  42\n', version+1)
    assert not publications(client.barrier())[path.as_uri()]['diagnostics']
    client.finish()
print('Unfinished-code protocol: collections/control/compound signatures, malformed boundaries, UTF-16, live imports, strict diagnostics, immutable buffers and no stale queries passed')
