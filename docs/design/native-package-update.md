# Named native dependency updates

Use `kalandra update` to deliberately refresh selected managed root dependencies
without editing their requested branch/tag or moving unrelated locked selections.

```sh
kalandra update geometry --project my-app/kalandra.toml
kalandra update geometry rendering --project my-app/kalandra.toml
```

Names are aliases in the root manifest's `[dependencies]`. At least one name is
required; unknown names, duplicates and more than 128 selections are rejected.
Local `[packages]` mappings are not managed Git dependencies. There is no implicit
update-all operation.

## Selection rules

An update requires a valid existing `kalandra.lock` matching the exact manifest
bytes and root requests. Run normal installation first if the manifest changed
or the project has no lock. Malformed locks are rejected rather than repaired.
`--frozen` and `--offline` cannot be combined with updates; ordinary installation
retains both modes.

Only selected root edges are unpinned for resolution. A branch/tag request selects
its current commit using the existing bounded Git resolver. An exact commit still
selects that exact commit. The manifest is not rewritten, package metadata versions
are not used to choose revisions, and version-range solving is not introduced.

Every unselected root keeps its existing pin, even if it uses the same repository
and requested ref as a selected alias. Two selected aliases of the same repository
and ref share the newly resolved selection. Distinct commits remain distinct
package identities under the existing package-local scope model.

An unchanged repository/commit identity retains its locked digest, entry and
private dependency edges. Updating a root whose ref still selects the same commit
does not recursively refresh its private refs. A newly selected owner commit can
resolve new private requests, while existing immutable package identities reused
by that graph retain their pins. This can add or remove graph nodes without
moving another owner's private versions.

To switch an exact revision or request a different tag/branch, edit the manifest
and run normal installation. Recursive/private-edge update selection needs a
separate interface; root aliases do not identify an arbitrary transitive owner.

## Validation and activation

Updates use the same installation guard, snapshot verification, graph limits,
reachable source/import validation and single-lock activation as normal installs.
They stage a complete lock and activate it only after checking that the captured
manifest and prior lock have not changed. Failed imports, ref resolution or guard
acquisition leave the active lock unchanged; an external actor's changes are not
overwritten. Unused immutable snapshots may remain cached after failure.

The activated lock can normalize serialization just as a normal install does.
No multi-file atomicity or fsync/power-loss durability is added. Compilation and
checking still consume the selected graph without fetching, updating or rewriting
locks. Source/import validation is not a promise of application type compatibility
with a changed library; check and build the application after updating.

## Qualification

[Update qualification](https://github.com/kalandra-lang/kalandra/blob/8ca7d50a3ab4b6cc611b5eb34bca7cff2d7933a7/native/metal-proof/evidence/native-package-update-2026-10-21.json)
records normal and ASan/UBSan installer API checks, the public CLI and relocated
Release CPU compiler. Local Git fixtures cover branch/tag moves, multiple aliases,
exact revisions, new graph nodes, unchanged-owner private pins, newly selected
owner commits, invalid requests/modes/locks, held guards and failed graph rollback.
Compiled worker programs verify the resulting root/private version choices.
[Installed Metal acceptance](native-metal-install.md#acceptance) also checks named
updates across CPU workers and GPU helpers, preserving private branch pins and
pre-update applications through offline relocation. Existing lock and Git-ref
qualification remains regression coverage. This does not
add authenticated/SSH transport, version-range solving, cache pruning or a
recursive update command. [Controlled HTTP/TLS update acceptance](native-remote-git.md#named-updates-over-https)
now checks selected refs, private pins, transport failures and offline worker
results through the existing remote transport policy.
