"""Independent Linux syscall oracle for a single checkpoint crash cut."""

import ast
import os
from pathlib import Path
import re


PHASES = ("before-staging-fsync", "after-staging-fsync", "after-rename", "after-parent-fsync")
SUCCESS = re.compile(r"^([a-z0-9]+)\((.*)\)\s+=\s+0(?:\s.*)?$")
QUOTED = re.compile(r'"(?:[^"\\]|\\.)*"')
FD = re.compile(r"^\d+<(.+)>$")


def operations(trace):
    """Read completed successful calls in a single strace -ff thread log."""
    for line in trace.read_text(errors="replace").splitlines():
        match = SUCCESS.fullmatch(line)
        if match is None:
            continue
        call, arguments = match.groups()
        if call == "fsync":
            descriptor = FD.fullmatch(arguments)
            if descriptor is not None:
                yield "fsync", (Path(descriptor[1]),)
            continue
        paths = tuple(Path(os.fsdecode(ast.literal_eval("b" + operand)))
                      for operand in QUOTED.findall(arguments))
        if call in ("mkdir", "mkdirat") and len(paths) == 1:
            yield "mkdir", paths
        elif call in ("rename", "renameat", "renameat2", "link", "linkat") and len(paths) == 2:
            yield "link" if call in ("link", "linkat") else "rename", paths


def assert_checkpoint_syscalls(trace_prefix, target, phase, staging=None):
    """Check only the writer that created this checkpoint's staging directory."""
    target = target.resolve()
    incarnation = target.parent
    parents = (incarnation, incarnation.parent, incarnation.parent.parent)
    staged = staging.resolve() if staging is not None else None
    traces = {path: list(operations(path)) for path in
              trace_prefix.parent.glob(f"{trace_prefix.name}.[0-9]*")}
    publishes = [(path, args[0].resolve()) for path, calls in traces.items()
                 for call, args in calls if call == "rename" and args[1].resolve() == target]
    published = phase in ("after-rename", "after-parent-fsync")
    if published:
        if staged is not None or len(publishes) != 1:
            raise RuntimeError(f"expected one successful rename to {target}: {publishes!r}")
        writer, staged = publishes[0]
    else:
        if staged is None or publishes:
            raise RuntimeError(f"unexpected checkpoint rename before {phase}: {publishes!r}")

    if staged.parent != incarnation or re.fullmatch(r"\.save-[A-Za-z0-9]{6}", staged.name) is None:
        raise RuntimeError(f"unexpected staging directory {staged} for {target}")

    creators = [path for path, calls in traces.items()
                if any(call == "mkdir" and args[0].resolve() == staged for call, args in calls)]
    if len(creators) != 1 or (published and creators[0] != writer):
        raise RuntimeError(f"could not identify a single checkpoint writer for {staged}: {creators!r}")
    writer = creators[0]

    selected = [
        ("fsync", (staged / "index.cagra",)),
        ("fsync", (staged / "manifest",)),
        ("fsync", (staged / "build.xid.tmp",)),
        ("marker", (staged / "build.xid.tmp", staged / "build.xid")),
        ("fsync", (staged,)),
        ("rename", (staged, target)),
        *(("fsync", (parent,)) for parent in parents),
    ]
    wanted = selected[:(4, 5, 6, 9)[PHASES.index(phase)]]
    actual = []
    created = False
    for call, args in traces[writer]:
        args = tuple(path.resolve() for path in args)
        if call == "mkdir" and args == (staged,):
            created = True
        if not created:
            continue
        if call in ("rename", "link") and args == selected[3][1]:
            call = "marker"
        step = (call, args)
        if step in selected:
            actual.append(step)
    if actual != wanted:
        def readable(sequence):
            return [(call, tuple(str(path) for path in args)) for call, args in sequence]
        raise RuntimeError(f"checkpoint syscall order at {phase} in {writer}: "
                           f"expected {readable(wanted)!r}, observed {readable(actual)!r}")
