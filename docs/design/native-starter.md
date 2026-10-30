# Starting a native project

Create an offline project with the installed native compiler:

```sh
kalandra new my-app
kalandra-check --project my-app/kalandra.toml
kalandra --project my-app/kalandra.toml -o my-app/program
./my-app/program
```

The result is `[2, 4, 6]`. No Bun, Node, package registry or Git setup is needed.
The same command is available in CPU-only and CPU/Metal compiler bundles.

The command creates exactly three files:

```text
my-app/
  kalandra.toml
  src/main.k
  src/math.k
```

`kalandra.toml` uses the existing native manifest format and selects `src/main.k`.
The entry imports an ordinary function from its sibling module and schedules
independent calls through the bounded CPU worker pool:

```kalandra
from "./math.k" import double

fn main() -> [int] ! TaskError:
  try parallelMap([1, 2, 3], double)
```

`src/math.k`:

```kalandra
export fn double(value: int) -> int:
  value * 2
```

Use `-O2`, `--sanitize`, or separate `--thread-sanitize` on the compile command
as usual. The starter uses CPU execution; selecting a Metal-capable compiler
package does not automatically place its tasks on GPU.

The parent directory must exist. The destination must not exist, including an
empty directory, regular file, symlink or dangling symlink. Paths with spaces,
Unicode and trailing separators work. Creation writes to a private sibling
staging directory, then publishes with an exclusive atomic rename on the current
macOS target. A concurrent creator cannot replace or merge an existing project.
Ordinary failures clean up the owned staging directory; an interrupted/killed
process can leave a `.kalandra-new-*` staging directory. No existing project is
modified, and no dependency hook or program is executed during creation.

This provides the initial new-project path under N3. It does not install packages,
resolve versions, create a lockfile, fetch dependencies, or change module visibility.
[Local package mappings](local-packages.md) remain explicit; the package-manager
workflow is still a separate implementation milestone.

The `native-starter-project` gate checks the generated project with the checker
and formatter, builds/runs it in normal, optimized, ASan/UBSan and TSan modes,
rejects existing destinations, and checks concurrent creation and staging cleanup.
The CPU/Metal installation gates also create and run a starter through a compiler
symlink from a relocated package outside the checkout.
