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
                             ('option', 'export enum Option[T]'), ('process', 'export type ExitStatus'), ('fs', 'export fn open'), ('parse', 'export fn parseI64'), ('path', 'export fn join'), ('env', 'export fn get')]:
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
with tempfile.TemporaryDirectory(dir=directory) as temp:
    root=Path(temp).resolve()
    path=root/'parsing.k'
    text='''from "std/parse" import parseI64 as parseCount, ParseError
fn main()->i64 ! ParseError:
  return try parseCount("42")
'''
    client=Client(server)
    client.init(rename=True,workspaceFolders=[{'uri':root.as_uri(),'name':'parsing'}])
    client.open(path,text)
    assert not publications(client.barrier())[path.as_uri()]['diagnostics']
    params={'textDocument':{'uri':path.as_uri()},'position':{'line':2,'character':14}}
    definition=client.rpc('textDocument/definition',params)['result']
    assert definition['uri']=='kalandra-stdlib:std/parse.k',definition
    hover=client.rpc('textDocument/hover',params)['result']
    assert 'String' in str(hover) and 'i64' in str(hover) and 'ParseError' in str(hover),hover
    signature=client.rpc('textDocument/signatureHelp',{
        **params,'position':{'line':2,'character':25}})['result']
    assert signature and 'text: String' in signature['signatures'][0]['label'],signature
    renamed=client.rpc('textDocument/rename',{**params,'newName':'readCount'})['result']
    changes=renamed['documentChanges']
    assert len(changes)==1 and changes[0]['textDocument']['uri']==path.as_uri() and len(changes[0]['edits'])==2,renamed
    declarations=client.rpc('kalandra/standardLibrary',{'uri':'kalandra-stdlib:std/parse.k'})['result']
    assert 'export error ParseError:' in declarations and 'export fn parseBool' in declarations,declarations
    client.finish()
print('PASS: parsing declarations, alias definition, hover, signature and local-only rename')
with tempfile.TemporaryDirectory(dir=directory) as temp:
    root=Path(temp).resolve();path=root/'paths.k'
    text='''from "std/path" import join as makePath, fileName
fn leaf(parts:[String])->String:
  return fileName(makePath(parts))
fn main()->unit:
  return ()
'''
    client=Client(server);client.init(rename=True,workspaceFolders=[{'uri':root.as_uri(),'name':'paths'}]);client.open(path,text)
    assert not publications(client.barrier())[path.as_uri()]['diagnostics']
    line=text.splitlines()[2];position=line.index('makePath')+2
    params={'textDocument':{'uri':path.as_uri()},'position':{'line':2,'character':position}}
    definition=client.rpc('textDocument/definition',params)['result']
    assert definition['uri']=='kalandra-stdlib:std/path.k',definition
    hover=client.rpc('textDocument/hover',params)['result']
    assert '[String]' in str(hover) and 'String' in str(hover),hover
    signature=client.rpc('textDocument/signatureHelp',{**params,'position':{'line':2,'character':line.index('parts')+2}})['result']
    assert signature and 'parts: [String]' in signature['signatures'][0]['label'],signature
    renamed=client.rpc('textDocument/rename',{**params,'newName':'buildPath'})['result']['documentChanges']
    assert len(renamed)==1 and renamed[0]['textDocument']['uri']==path.as_uri() and len(renamed[0]['edits'])==2,renamed
    declarations=client.rpc('kalandra/standardLibrary',{'uri':'kalandra-stdlib:std/path.k'})['result']
    assert 'export fn isAbsolute(path: String) -> bool:' in declarations,declarations
    client.finish()
print('PASS: path declarations, aliases, definition URI, signature and hover')

with tempfile.TemporaryDirectory(dir=directory) as temp:
    root=Path(temp).resolve()
    path=root/'display.k'
    text='''from "std/fmt" import format, Display
type Point:
  x:i64
impl Display for Point:
  fn display(value:Self)->String:
    return format("{}",value.x)
fn main()->String:
  return format("{:>8}",Point {x:42})
'''
    client=Client(server)
    client.init(workspaceFolders=[{'uri':root.as_uri(),'name':'display'}])
    client.open(path,text)
    assert not publications(client.barrier())[path.as_uri()]['diagnostics']
    line=text.splitlines()[3]
    definition=client.rpc('textDocument/definition',{
        'textDocument':{'uri':path.as_uri()},
        'position':{'line':3,'character':line.index('Display')+2}})['result']
    assert definition['uri']=='kalandra-stdlib:std/fmt.k',definition
    declarations=client.rpc('kalandra/standardLibrary',{'uri':'kalandra-stdlib:std/fmt.k'})['result']
    assert 'export trait Display:' in declarations and 'fn display(value: Self) -> String' in declarations,declarations
    client.finish()
print('PASS: custom Display conformance and canonical standard trait definition')

with tempfile.TemporaryDirectory(dir=directory) as temp:
    root=Path(temp).resolve()
    path=root/'directory.k'
    text='''from "std/fs" import readDir as entries, metadata, FsError
fn main()->unit ! FsError:
  cursor=try entries(".")
  info=try metadata(".")
  return ()
'''
    client=Client(server)
    client.init(workspaceFolders=[{'uri':root.as_uri(),'name':'directory'}])
    client.open(path,text)
    diagnostics=publications(client.barrier())[path.as_uri()]['diagnostics']
    assert not diagnostics,diagnostics
    for row,word in [(2,'entries'),(3,'metadata')]:
        line=text.splitlines()[row]
        params={'textDocument':{'uri':path.as_uri()},
                'position':{'line':row,'character':line.index(word)+1}}
        definition=client.rpc('textDocument/definition',params)['result']
        assert definition['uri']=='kalandra-stdlib:std/fs.k',definition
        signature=client.rpc('textDocument/signatureHelp',{
            **params,'position':{'line':row,'character':line.index('(".")')+2}})['result']
        assert signature and 'path: String' in signature['signatures'][0]['label'],signature
    declarations=client.rpc('kalandra/standardLibrary',{'uri':'kalandra-stdlib:std/fs.k'})['result']
    assert 'export fn readDir' in declarations and 'export type DirEntry:' in declarations,declarations
    client.finish()
print('PASS: directory iterator/metadata aliases, canonical definitions and editor signatures')


with tempfile.TemporaryDirectory(dir=directory) as temp:
    root=Path(temp).resolve()
    path=root/'equality.k'
    text='''from "std/cmp" import Equal as Same
type Position:
  x:i64
impl Same for Position:
  fn equal(left:Self,right:Self)->bool:
    return left.x==right.x
fn main()->bool:
  return Position {x:42}==Position {x:42}
'''
    client=Client(server)
    client.init(workspaceFolders=[{'uri':root.as_uri(),'name':'equality'}])
    client.open(path,text)
    diagnostics=publications(client.barrier())[path.as_uri()]['diagnostics']
    assert not diagnostics,diagnostics
    definition=client.rpc('textDocument/definition',{
        'textDocument':{'uri':path.as_uri()},
        'position':{'line':3,'character':7}})['result']
    assert definition['uri']=='kalandra-stdlib:std/cmp.k',definition
    declarations=client.rpc('kalandra/standardLibrary',{'uri':'kalandra-stdlib:std/cmp.k'})['result']
    assert 'export trait Equal:' in declarations and 'fn equal(left: Self, right: Self) -> bool' in declarations,declarations
    client.finish()
print('PASS: canonical Equal trait declarations, alias definition and operator checking')

with tempfile.TemporaryDirectory(dir=directory) as temp:
    root=Path(temp).resolve()
    path=root/'ordering.k'
    text='''from "std/cmp" import Order as Rank, Ordering, compare as compareValues, sortedBy as ranked
type Score:
  value:i64
impl Rank for Score:
  fn compare(a:Self,b:Self)->Ordering:
    return compareValues(a.value,b.value)
fn main()->unit:
  result=ranked([Score {value:2},Score {value:1}],compareValues[Score])
  return ()
'''
    client=Client(server)
    client.init(workspaceFolders=[{'uri':root.as_uri(),'name':'ordering'}])
    client.open(path,text)
    diagnostics=publications(client.barrier())[path.as_uri()]['diagnostics']
    assert not diagnostics,diagnostics
    for row,word in [(3,'Rank'),(5,'compareValues'),(7,'ranked')]:
        definition=client.rpc('textDocument/definition',{
            'textDocument':{'uri':path.as_uri()},
            'position':{'line':row,'character':text.splitlines()[row].index(word)+1}})['result']
        assert definition['uri']=='kalandra-stdlib:std/cmp.k',definition
    declarations=client.rpc('kalandra/standardLibrary',{'uri':'kalandra-stdlib:std/cmp.k'})['result']
    assert 'export trait Order:' in declarations and 'export enum Ordering:' in declarations,declarations
    for name in ['compare','totalCompare','sortedBy','sortBy']:
        assert 'export fn '+name+'[' in declarations,(name,declarations)
    client.finish()
print('PASS: canonical Order/Ordering declarations, aliases and sorting editor definitions')
