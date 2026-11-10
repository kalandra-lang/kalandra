"""Read-only standard declarations, real definition URIs and variadic formatting guidance."""
from pathlib import Path
import sys
import tempfile
from ProtocolClient import Client, publications

server, directory = sys.argv[1:]
Path(directory).mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=directory) as temp:
    root = Path(temp).resolve()
    path = root / 'main.k'
    text = '''from "std/io" import println, IoError
from "std/fmt" import format as fmt
fn main()->unit ! IoError:
  try println(fmt("{}:{}",42,true))
  return ()
'''
    client = Client(server)
    client.init(rename=True, workspaceFolders=[{'uri': root.as_uri(), 'name': 'test'}])
    client.open(path, text)
    assert not publications(client.barrier())[path.as_uri()]['diagnostics']
    for module, expected in [('io', 'export fn println'), ('fmt', 'export fn format'),
                             ('process', 'export type ExitStatus'), ('fs', 'export fn open')]:
        result = client.rpc('kalandra/standardLibrary', {'uri': f'kalandra-stdlib:std/{module}.k'})
        assert expected in result['result'], result
    for uri in ('file:///etc/passwd', 'kalandra-stdlib:std/missing.k', 'kalandra-stdlib:std/../main.k'):
        assert client.rpc('kalandra/standardLibrary', {'uri': uri})['error']['code'] == -32602
    for word, target in [('println', 'io'), ('fmt', 'fmt')]:
        line = text.splitlines()[3]
        pos = line.index(word)
        params = {'textDocument': {'uri': path.as_uri()}, 'position': {'line': 3, 'character': pos}}
        result = client.rpc('textDocument/definition', params)['result']
        assert result['uri'] == f'kalandra-stdlib:std/{target}.k', result
        if word == 'fmt':
            hover = client.rpc('textDocument/hover', params)['result']
            assert 'values...' in str(hover) and 'String' in str(hover), hover
            signature = client.rpc('textDocument/signatureHelp', {
                **params, 'position': {'line': 3, 'character': line.index('true') + 1}})['result']
            assert signature and 'values...' in signature['signatures'][0]['label'], signature
        # A local alias can change without editing compiler declarations.
        renamed = client.rpc('textDocument/rename', {**params, 'newName': 'changed'})
        if word == 'fmt':
            changes = renamed['result']['documentChanges']
            assert len(changes) == 1 and changes[0]['textDocument']['uri'] == path.as_uri(), renamed
            assert len(changes[0]['edits']) == 2, renamed
        else:
            assert 'error' in renamed or renamed.get('result') is None, renamed
    # Editing assistance uses the resolved intrinsic even before the arguments
    # are complete. It must not grant the same signature to a shadowing name.
    for version, suffix, active in [(2, 'fmt(', 0), (3, 'fmt("{}",', 1),
                                    (4, 'fmt("{}:{}",42,', 1)]:
        changed = '\n'.join(text.splitlines()[:2]) + '\nfn main()->unit:\n  value=' + suffix
        client.change(path, changed, version)
        client.barrier()
        signature = client.rpc('textDocument/signatureHelp', {
            'textDocument': {'uri': path.as_uri()},
            'position': {'line': 3, 'character': len(changed.splitlines()[3])}})['result']
        assert signature and 'values...' in signature['signatures'][0]['label'], (version, changed, signature)
        assert signature['activeParameter'] == active, signature
    client.finish()
with tempfile.TemporaryDirectory(dir=directory) as temp:
    root = Path(temp).resolve()
    path = root / 'files.k'
    text = '''from "std/fs" import File, BufferedReader, FsError, read as readBytes
fn prefix(reader: BufferedReader)->i64 ! FsError:
  chunk=try readBytes(reader,4)
  return length(chunk)
fn consume(own file:File)->unit:
  return ()
fn main()->unit:
  return ()
'''
    client=Client(server)
    client.init(rename=True, workspaceFolders=[{'uri':root.as_uri(),'name':'files'}])
    client.open(path,text)
    assert not publications(client.barrier())[path.as_uri()]['diagnostics']
    params={'textDocument':{'uri':path.as_uri()},'position':{'line':2,'character':14}}
    definition=client.rpc('textDocument/definition',params)['result']
    assert definition['uri']=='kalandra-stdlib:std/fs.k',definition
    signature=client.rpc('textDocument/signatureHelp',{
        **params,'position':{'line':2,'character':29}})['result']
    assert signature and 'BufferedReader' in signature['signatures'][0]['label'],signature
    hover=client.rpc('textDocument/hover',{
        **params,'position':{'line':4,'character':5}})['result']
    assert hover and 'own file: File' in str(hover),hover
    # The virtual filesystem module remains readable but cannot be renamed.
    declarations=client.rpc('kalandra/standardLibrary',{'uri':'kalandra-stdlib:std/fs.k'})['result']
    assert 'export fn writeLine' in declarations and 'own file: File' in declarations,declarations
    for version,suffix in [(2,'readBytes('),(3,'readBytes(reader,')]:
        changed=text.split('fn prefix',1)[0]+'fn prefix(reader: BufferedReader)->unit:\n  value='+suffix
        client.change(path,changed,version)
        client.barrier()
        result=client.rpc('textDocument/signatureHelp',{
            **params,'position':{'line':2,'character':len(changed.splitlines()[2])}})['result']
        assert result and 'maxBytes: u64' in result['signatures'][0]['label'],result
        if version==3: assert 'BufferedReader' in result['signatures'][0]['label'],result
    client.finish()
print('PASS: read-only standard declarations, definition URIs, format hover/signature and bounded content requests')
