"""Execute authored documentation in isolated directories. Invoked by verify-docs.ts.

The fixture contains expected outcomes, never duplicate source. GPU execution is
explicit because it requires a Metal-enabled compiler and physical Apple hardware.
OS/toolchain setup recipes and the PowerShell variant are reported separately.
"""
import argparse
import concurrent.futures
import hashlib
import platform
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gpu', action='store_true')
parser.add_argument('--tools', type=Path, default=ROOT / 'build/native/native')
parser.add_argument('--report', type=Path)
args = parser.parse_args()
PAGES = json.load(sys.stdin)
PLAN = json.loads((ROOT / 'tests/fixtures/docs-examples.json').read_text())
TOOLS = args.tools.resolve()
ENV = dict(os.environ, PATH=str(TOOLS) + os.pathsep + os.environ['PATH'],
           GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1',
           GIT_TERMINAL_PROMPT='0')
RESULTS = []
COVERED = set()


def command(argv, cwd, expected=None):
    result = subprocess.run(list(map(str, argv)), cwd=cwd, env=ENV,
                            capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise AssertionError(f'{argv}\n{result.stdout}\n{result.stderr}')
    if expected is not None:
        assert result.stdout.strip() == expected, (argv, expected, result.stdout)
    return result.stdout.strip()


def blocks(page, language):
    return [(i, b['source']) for i, b in enumerate(PAGES[page]) if b['language'] == language]


def shell(page, index, cwd, expected=None):
    i, source = blocks(page, 'bash')[index]
    result = command(['sh', '-eu', '-c', source], cwd)
    if expected is not None:
        assert result == expected or result.splitlines()[-1] == expected, (page, index, expected, result)
    COVERED.add((page, i))
    RESULTS.append({'page': page, 'block': i, 'kind': 'shell', 'status': 'passed', 'expected': expected, 'output': result})
    return result


def write(path, source):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source)


class RenderedCode(HTMLParser):
    def __init__(self):
        super().__init__()
        self.active = False
        self.blocks = []
        self.parts = []

    def handle_starttag(self, tag, attrs):
        if tag == 'pre':
            self.active = True
            self.parts = []

    def handle_endtag(self, tag):
        if tag == 'pre':
            self.active = False
            self.blocks.append(''.join(self.parts).strip())

    def handle_data(self, data):
        if self.active:
            self.parts.append(data)


assert set(PAGES) == set(PLAN), 'Add expected outcomes for every documentation page'
for page, rows in PAGES.items():
    html = ROOT / 'apps/website/build' / ('docs.html' if page == 'index' else f'docs/{page}.html')
    parsed = RenderedCode()
    parsed.feed(html.read_text())
    assert [r['source'].strip() for r in rows] == parsed.blocks, f'Rendered source drift: {page}'
    assert len(blocks(page, 'kalandra')) == len(PLAN[page]), f'Unclassified native example: {page}'

with tempfile.TemporaryDirectory(prefix='kalandra-docs-') as temporary:
    BASE = Path(temporary).resolve()

    def native(item):
        page, ordinal, index, source, plan = item
        if plan['kind'] == 'workflow':
            return None  # Executed below with the page's actual files and manifest.
        directory = BASE / page / str(ordinal)
        write(directory / 'main.k', source)
        if page in ['getting-started', 'modules']:
            libraries = [s for _, s in blocks(page, 'kalandra') if s.startswith('export ')]
            assert len(libraries) == 1
            write(directory / 'math.k', libraries[0])
        if plan['kind'] == 'reject':
            p = subprocess.run([str(TOOLS / 'kalandra-check'), str(directory / 'main.k')],
                               env=ENV, capture_output=True, text=True, timeout=30)
            assert p.returncode != 0 and 'S010' in p.stderr and 'expected int64, got int32' in p.stderr, p.stderr
            return {'page': page, 'block': index, 'kind': 'expected-diagnostic', 'status': 'passed'}
        flags = ['--placement'] if plan['kind'] == 'gpu' else []
        command([TOOLS / 'kalandra-check', *flags, directory / 'main.k'], directory)
        if source.startswith('export '):
            return {'page': page, 'block': index, 'kind': 'companion-module', 'status': 'passed'}
        if plan['kind'] == 'gpu' and not args.gpu:
            return {'page': page, 'block': index, 'kind': 'gpu', 'status': 'not-run', 'reason': 'Requires --gpu on Apple Metal hardware'}
        modes = ['-O2'] if plan['kind'] == 'gpu' else ['-O0', '-O2']
        outputs = {}
        for mode in modes:
            output = directory / ('app' if plan['kind'] == 'gpu' else 'program')
            compile_flags = ['--target', 'metal'] if plan['kind'] == 'gpu' else []
            command([TOOLS / 'kalandra', directory / 'main.k', *compile_flags, mode, '-o', output], directory)
            binary = output / 'program' if plan['kind'] == 'gpu' else output
            outputs[mode] = command([binary], directory, plan['expected'])
        return {'page': page, 'block': index, 'kind': 'gpu' if plan['kind'] == 'gpu' else 'cpu', 'status': 'passed', 'expected': plan['expected'], 'outputs': outputs}

    items = [(page, n, i, source, PLAN[page][n]) for page in PAGES
             for n, (i, source) in enumerate(blocks(page, 'kalandra'))]
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for result in pool.map(native, items):
            if result:
                RESULTS.append(result)
                COVERED.add((result['page'], result['block']))

    # Actual check/compile/run instructions from the introductory and module pages.
    for page in ['index', 'modules']:
        n = next(x for x in items if x[0] == page and 'fn main(' in x[3])
        shell(page, 0, BASE / page / str(n[1]), PLAN[page][n[1]]['expected'])
    parallel = next(x for x in reversed(items) if x[0] == 'parallelism')
    shell('parallelism', 0, BASE / 'parallelism' / str(parallel[1]), parallel[4]['expected'])
    for page in ['gpu', 'gpu-numerics']:
        for shell_index, (block_index, _) in enumerate(blocks(page, 'bash')):
            # Run each invocation against the preceding authored program, so
            # policy examples and later programs cannot silently escape checks.
            program = max((item for item in items if item[0] == page and item[2] < block_index),
                          key=lambda item: item[2])
            if args.gpu:
                shell(page, shell_index, BASE / page / str(program[1]), program[4]['expected'])
            else:
                RESULTS.append({'page': page, 'block': block_index, 'kind': 'shell',
                                'status': 'not-run', 'reason': 'Requires --gpu'})
                COVERED.add((page, block_index))

    # The documented starter, its exact sibling module, and an edited rebuild.
    starter = BASE / 'starter'
    starter.mkdir()
    shell('getting-started', 0, starter, '[2, 4, 6]')
    for name, (_, source) in zip(['main.k', 'math.k'], blocks('getting-started', 'kalandra')):
        assert (starter / 'my-app/src' / name).read_text().strip() == source.strip()
    math = starter / 'my-app/src/math.k'
    math.write_text(math.read_text().replace('value * 2', 'value * 3'))
    shell('getting-started', 1, starter)
    command([starter / 'my-app/program'], starter, '[3, 6, 9]')

    # Minimal project, local mapped module, and optimized mode.
    project = BASE / 'project'
    sources = blocks('projects', 'kalandra')
    tomls = blocks('projects', 'toml')
    write(project / 'my-app/src/main.k', sources[0][1])
    write(project / 'my-app/kalandra.toml', tomls[0][1])
    shell('projects', 0, project, '42')
    write(project / 'my-app/lib/geometry.k', sources[1][1])
    write(project / 'my-app/src/main.k', sources[2][1])
    write(project / 'my-app/kalandra.toml', tomls[1][1])
    shell('projects', 0, project, '42')
    shell('projects', 1, project, '42')

    # Real local Git installation, frozen/offline graph reuse, then a named update.
    packages = BASE / 'packages-workflow'
    packages.mkdir()
    shell('packages', 0, packages)
    write(packages / 'geometry/kalandra.toml', blocks('packages', 'toml')[0][1])
    write(packages / 'geometry/src/public.k', blocks('packages', 'kalandra')[0][1])
    write(packages / 'my-app/src/main.k', blocks('packages', 'kalandra')[1][1])
    shell('packages', 1, packages)
    shell('packages', 2, packages, '42')
    lock = packages / 'my-app/kalandra.lock'
    before = lock.read_bytes()
    shell('packages', 3, packages)
    assert lock.read_bytes() == before, 'Frozen installs changed the lock'
    module = packages / 'geometry/src/public.k'
    module.write_text(module.read_text().replace('value * 2', 'value * 3'))
    shell('packages', 4, packages, '63')
    assert lock.read_bytes() != before, 'Named update did not refresh the pin'

    # Exercise the standalone source-library manifest from the projects page too.
    library = packages / 'geometry'
    write(library / 'kalandra.toml', tomls[2][1])
    write(library / 'src/public.k', sources[1][1])
    command(['git', '-C', library, 'add', '.'], packages)
    command(['git', '-C', library, '-c', 'user.name=Docs Example', '-c', 'user.email=docs@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-m', 'Source library manifest'], packages)
    write(packages / 'my-app/src/main.k', sources[2][1])
    command([TOOLS / 'kalandra', 'update', 'geometry', '--project', 'my-app/kalandra.toml'], packages)
    command([TOOLS / 'kalandra-check', '--project', 'my-app/kalandra.toml'], packages)
    command([TOOLS / 'kalandra', '--project', 'my-app/kalandra.toml', '-o', 'my-app/program'], packages)
    command([packages / 'my-app/program'], packages, '42')
    for page in ['projects', 'packages']:
        for i, _ in blocks(page, 'kalandra') + blocks(page, 'toml'):
            COVERED.add((page, i))
            RESULTS.append({'page': page, 'block': i, 'kind': 'project-file', 'status': 'passed'})

    # JSON diagnostics, formatter check/stdout, optimized compilation and IR outputs.
    tooling = BASE / 'tooling'
    write(tooling / 'main.k', blocks('index', 'kalandra')[0][1])
    command([TOOLS / 'kalandra', 'new', 'my-app'], tooling)
    shell('tooling', 0, tooling)
    json_command = blocks('tooling', 'bash')[0][1].splitlines()[-1]
    assert json.loads(command(['sh', '-eu', '-c', json_command], tooling)) is not None
    shell('tooling', 1, tooling, blocks('index', 'kalandra')[0][1].strip())
    shell('tooling', 2, tooling)
    shell('tooling', 3, tooling)
    for filename in ['program.mlir', 'program.ll']:
        assert (tooling / filename).stat().st_size > 0
    command([tooling / 'program'], tooling, '42')

    # These describe provisioning a machine / an alternate OS, not sample programs.
    for page in PAGES:
        for i, block in enumerate(PAGES[page]):
            if (page, i) in COVERED:
                continue
            if page in ['installation', 'contributing'] and block['language'] == 'bash':
                command(['sh', '-n', '-c', block['source']], BASE)
                reason = 'Environment/contributor workflow recipe; syntax checked, full setup/build is not run by the examples verifier'
            elif page == 'cross-compilation' and block['language'] == 'bash':
                command(['sh', '-n', '-c', block['source']], BASE)
                reason = 'Target SDK recipe; compiled programs and commands are exercised by native cross-platform qualification'
            elif page == 'cross-compilation' and block['language'] == 'json':
                profile = json.loads(block['source'])
                assert profile['format'] == 1 and profile['target'] == 'aarch64-unknown-linux-gnu'
                assert set(profile) == {'format', 'target', 'sysroot', 'gccDirectory', 'runtimePackage', 'linker'}
                reason = 'Toolchain profile template; native cross-platform qualification supplies the actual SDK/package paths'
            elif block['language'] == 'powershell':
                reason = 'Windows-only invocation; not executed on this POSIX host'
            else:
                raise AssertionError(f'Unverified code block: {page}:{i} {block["language"]}')
            RESULTS.append({'page': page, 'block': i, 'kind': block['language'], 'status': 'prerequisite-recipe', 'reason': reason})

for row in RESULTS:
    if 'block' in row:
        source = PAGES[row['page']][row['block']]['source']
        row['sourceSHA256'] = hashlib.sha256(source.encode()).hexdigest()

summary = {'host': platform.platform(),
           'toolsSHA256': {name: hashlib.sha256((TOOLS / name).read_bytes()).hexdigest() for name in ['kalandra', 'kalandra-check', 'kalandra-fmt']},
           'pages': len(PAGES), 'nativeExamples': len(items),
           'renderedBlocks': sum(map(len, PAGES.values())),
           'cpuPrograms': sum('fn main(' in source and plan['kind'] not in ['reject', 'gpu'] for _, _, _, source, plan in items),
           'gpuPrograms': sum(r['kind'] == 'gpu' and r['status'] == 'passed' for r in RESULTS),
           'results': RESULTS}
if args.report:
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps({k: v for k, v in summary.items() if k != 'results'}, indent=2))
for row in RESULTS:
    if row['status'] in ['not-run', 'prerequisite-recipe']:
        print(f'{row["page"]}: {row["reason"]}')
