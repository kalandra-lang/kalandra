"""Inspect emitted entry reachability, including callback/descriptor references."""
import re


def reachable_symbols(text, entry="kalandra_task_test_entry"):
    bodies = {}
    current = None
    for line in text.splitlines():
        if line.startswith("define "):
            names = re.findall(r"@([A-Za-z0-9_.$-]+)", line)
            assert names, line
            current = names[0]
            bodies[current] = [line]
        elif current is not None:
            bodies[current].append(line)
            if line == "}":
                current = None
        elif line.startswith("@") and " = " in line:
            bodies[line[1:].split(" = ", 1)[0]] = [line]
    assert entry in bodies, "missing selected task entry"
    seen, pending = set(), [entry]
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        pending.extend(
            re.findall(r"@([A-Za-z0-9_.$-]+)", "\n".join(bodies.get(name, ())))
        )
    return seen


def assert_fused_entry(text, name):
    reachable = reachable_symbols(text)
    dynamic = sorted(
        symbol
        for symbol in reachable
        if symbol.startswith(("kalandra_generator_", "kalandra_iterator_"))
    )
    assert not dynamic, (
        "local fusion fell back to dynamic cursor execution",
        name,
        dynamic,
    )
