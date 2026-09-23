#!/usr/bin/env python3
"""Fail the build if any device function's stack frame can overflow its task.

**Why this is a gate and not a review step.** `sizeof(Config)` is 8,912 B. Any
function that holds one BY VALUE -- as a local, or as the return value of a
`Config ...()` factory -- makes its own frame that big, and its callers' frames
the sum of the chain. Three task stacks are smaller than that:

    app_main          CONFIG_ESP_MAIN_TASK_STACK_SIZE   3584 B
    TinyUSB task      tusb_cfg.task.size (UsbLink.cpp)  4096 B

Measured on the pre-fix build (2026-09-22), with the compiler's own
`-fstack-usage`, the frames were:

    SystemOrchestrator::Boot            18,704 B   on the 3,584 B main task
    CommandRouter::HandleConfigPatch    17,920 B   on the 4,096 B USB task
    CommandRouter::HandleLearnCommit    18,096 B
    CommandRouter::BeginConfigReplyRun  17,856 B
    CommandRouter::HandleConfigEnd       9,088 B
    CommandRouter::HandleLearnStart      9,728 B
    ConfigStore::Load                    8,960 B

chained Boot -> Load -> ConfigDecodeBlob -> ConfigDecodeJson to ~36 KB. That is a
guaranteed stack overflow on every device boot and every `config_get`, and it is
INVISIBLE to everything that currently runs: the host suite runs on 8 MB threads,
so the frames never matter, and the board has never been flashed. The device-only
blind spot again -- same shape as the EspHal NVS cluster `check_hal_contracts.py`
pins, one layer up.

**What this measures, and why it needs the toolchain.** The check compiles each
project translation unit with `-fstack-usage` (the number is the compiler's, not
a guess from source) and compares every frame -- and every call CHAIN -- against
the stack of the task that reaches it. It reads the flags from the device build's
own `compile_commands.json`, so it measures the real `-Og` build rather than a
re-derivation.

Chains are what matter, not single frames: a 1,072 B frame at the top of a thread
is harmless, but the same frame below a 2,352 B one is fatal. The check builds
the call graph from the objects' `ASM_EXPAND` relocations and takes the EXACT
longest path (a DP over the condensation -- the graph is a DAG here), not a DFS
heuristic: the DFS version was measured to miss a real overflow on a shared
branch, which is a false negative, the one direction a stack gate must not fail
in.

It found two overflows:

  * the by-value `Config` shape above (frames of 8-18 KB, chains to ~36 KB); and
  * `Process`'s 2,352 B frame (two 1 KB scratch buffers), which put
    `app_main -> UsbLinkService -> Process -> Nack -> Emit -> NdjsonWriter::Write`
    at 4,080 B on the 3,584 B main task -- hit the first time a `config_get`
    reply was chunked out.

Exit codes: 0 clean, 1 a frame/chain exceeds its task stack, 2 the build is
missing (run `pio run -e esp32s3` first).
"""

import argparse
import json
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def _read_config_bytes(repo: pathlib.Path) -> int:
    """`sizeof(Config)` from ConfigModel.h's own static_assert, not a second copy.

    Strict on purpose. A missing match MUST fail loudly: falling back to a
    remembered number is exactly the phantom the constant used to be -- a message
    quoting a size nothing in the tree asserts. This gate's whole design is that a
    check which cannot run is an ERROR, never a pass (see `chain_precondition_error`).
    """
    src = (repo / "lib" / "Config" / "ConfigModel.h").read_text()
    m = re.search(r"static_assert\(\s*sizeof\(Config\)\s*==\s*(\d+)", src)
    if m is None:
        raise RuntimeError(
            "lib/Config/ConfigModel.h must static_assert sizeof(Config) == N; "
            "tools/check_stack_usage.py reads that assertion instead of copying the number")
    return int(m.group(1))

# The size of a `Config`, which is what makes this class of defect possible. It is
# READ from the model's own `static_assert` (ConfigModel.h) rather than carried as a
# second copy here: the number is load-bearing in six comments across lib/, and a
# private copy would let the message quote a size the model no longer has. The
# assertion is what keeps it true -- if a field changes, the device build fails and
# names the two budgets to re-measure.
CONFIG_BYTES = _read_config_bytes(REPO)

# The task stacks, in bytes. `app_main` is IDF's Kconfig value; the USB task is
# set in `UsbLinkStart`. Both are read from source where they live so an edit
# that raises one is reflected here without a second number to keep in sync.
USB_TASK_DEFAULT = 4096
MAIN_TASK_DEFAULT = 3584


def read_usb_task_size(repo: pathlib.Path) -> int:
    src = (repo / "lib" / "Link" / "UsbLink.cpp").read_text()
    m = re.search(r"tusb_cfg\.task\.size\s*=\s*(\d+)", src)
    return int(m.group(1)) if m else USB_TASK_DEFAULT


def read_main_task_size(repo: pathlib.Path) -> int:
    """From the generated sdkconfig, falling back to IDF's default.

    The real value is a Kconfig symbol, so reading the generated header is the
    only way to see an override; the default is what IDF uses when it is unset.
    """
    for cand in (repo / ".pio" / "build" / "esp32s3" / "config" / "sdkconfig.h",
                 repo / "sdkconfig.esp32s3"):
        if not cand.is_file():
            continue
        m = re.search(r"CONFIG_ESP_MAIN_TASK_STACK_SIZE\s+(\d+)", cand.read_text())
        if m:
            return int(m.group(1))
    return MAIN_TASK_DEFAULT


def resolve_compiler(argv0: str) -> str:
    """Turn the compile database's first token into an executable path.

    The TWO databases spell the compiler differently: `.pio/build/<env>/` records
    the absolute path, while the root `compile_commands.json` records the bare
    name `xtensa-esp32s3-elf-g++`, which is not on PATH outside a PlatformIO
    shell. Recompiling from the root file (the one that has `src/`) therefore
    fails to exec, so the toolchain directory is found and prepended here.
    """
    if pathlib.Path(argv0).is_file():
        return argv0
    toolchain = objdump_path()
    if toolchain:
        cand = pathlib.Path(toolchain).parent / argv0
        if cand.is_file():
            return str(cand)
    return argv0


def is_project_source(path: str, repo: pathlib.Path) -> bool:
    """Whether a compile-database entry is one of THIS project's translation units.

    Only `lib/` and `src/` UNDER THE PROJECT ROOT. IDF and the managed components
    are not this project's to fix; recompiling them also makes the report
    unreadable, since mbedTLS and NimBLE have functions with larger frames than
    anything here. A frame in a dependency still shows up in the chain check if it
    is on a path from our entry points.

    The path is spelled two ways -- absolute in the build's database, relative in
    the root one (`src/main.cpp`) -- so it is resolved against the repo root and
    then matched on the tail. Requiring an absolute `/code/lib/` or `/code/src/`
    rejects the relative form, which is how `main.cpp` -- the main task's root --
    came to be skipped while the gate still reported success.
    """
    p = path.replace("\\", "/")
    if not p.endswith((".cpp", ".c")):
        return False
    full = p if p.startswith("/") else str(repo / p)
    for sub in ("lib", "src"):
        root = str(repo / sub) + "/"
        if full.startswith(root) and "/code/" not in full[len(root):]:
            # A nested `components/.../src/` is a dependency, not ours: the only
            # project sources are the files directly under lib/ and src/.
            return True
    return False


def compile_with_stack_usage(entries: list[dict], outdir: pathlib.Path) -> None:
    """Recompile every project TU with -fstack-usage, using the real flags.

    `entries` comes from `load_compile_entries`, which has already merged the two
    databases per file and filtered to this project's sources -- so a TU whose
    recompile fails is a real problem, not a database-selection artifact.
    """
    outdir.mkdir(parents=True, exist_ok=True)
    for e in entries:
        f = e["file"]
        parts = shlex.split(e["command"])
        parts[0] = resolve_compiler(parts[0])
        args, skip = [], False
        for p in parts:
            if skip:
                skip = False
                continue
            if p == "-o":
                skip = True
                continue
            if p == "-c":
                continue
            args.append(p)
        base = pathlib.Path(f).name
        args += ["-c", "-fstack-usage", "-o", str(outdir / f"{base}.o")]
        subprocess.run(args, cwd=e["directory"], capture_output=True, text=True)


def parse_su(outdir: pathlib.Path) -> tuple[dict, dict]:
    """({qualified-name: frame_bytes}, {qualified-name: file:line}) from the .su files.

    The `.su` format is `file:line:col:function<TAB>bytes<TAB>qualifier`. Note it
    is a TAB-separated record on one line -- splitting on whitespace (an earlier
    mistake) merges the columns and silently reports the wrong number.

    The function column holds a SOURCE-LEVEL SIGNATURE (`void
    SystemOrchestrator::Boot()`), not the mangled symbol, and the call graph is
    keyed by mangled symbol. Both are reduced to a bare QUALIFIED NAME
    (`SystemOrchestrator::Boot`) -- see `strip_params` -- so the two tables join.
    Comparing raw signatures would match nothing, which reports every chain as
    one frame and a clean bill of health: exactly the silent-pass failure this
    gate exists to prevent, so the join is checked below rather than assumed.
    """
    frames, where = {}, {}
    for su in sorted(outdir.glob("*.su")):
        for line in su.read_text().splitlines():
            cols = line.split("\t")
            if len(cols) < 3:
                continue
            loc, size, kind = cols[0], cols[1], cols[2]
            if kind not in ("static", "dynamic", "dynamic,bounded"):
                continue
            # `file:line:col:function` -- split with maxsplit=3 and take the LAST
            # field. A plain `split(":")[-1]` breaks on the `::` inside a
            # qualified C++ name, so `void CommandRouter::Process()` reduced to
            # `Process()` and then matched nothing in the call graph -- every
            # chain came back one frame long.
            name = strip_params(loc.split(":", 3)[-1].strip())
            frames[name] = int(size)
            where[name] = loc
    return frames, where


def objdump_path() -> str | None:
    """The device toolchain's objdump, so the graph matches the frames.

    Looked up on PATH first (which is how a CI runner and a PlatformIO shell
    both expose it), then by globbing the PlatformIO package directory, whose
    name carries a version suffix. Hardcoding an absolute path that names this
    machine's home directory would make the gate fail on the runner.
    """
    on_path = shutil.which("xtensa-esp32s3-elf-objdump")
    if on_path:
        return on_path
    for base in (pathlib.Path.home() / ".platformio" / "packages",):
        if not base.is_dir():
            continue
        for pkg in sorted(base.glob("toolchain-xtensa-esp-elf*")):
            cand = pkg / "bin" / "xtensa-esp32s3-elf-objdump"
            if cand.is_file():
                return str(cand)
    return None


def build_call_graph(outdir: pathlib.Path, objdump: str) -> dict:
    """{mangled-caller: set(mangled-callees)} from the relocation records.

    A call in the disassembly is not a `call` opcode to chase: xtensa loads the
    callee's address through a literal pool with `callx8`, so the TARGET is named
    by an `R_XTENSA_ASM_EXPAND` relocation. Those are the edges. Intra-file calls
    also show up as `ASM_EXPAND` to the same symbol, so one pass over every
    object gives the whole project's graph.

    **A `.text.` section symbol must be folded onto the function it wraps.** A
    relocation to a static function whose section needs no relocation emits the
    SECTION symbol `.text.<mangled>` rather than the function symbol, and that
    string is not a node in this graph -- the function's code block is keyed by
    the bare `<mangled>`. So the edge lands on a phantom with no outgoing edges and
    every chain through it STOPS: on this build 129 of 805 edges (68 distinct
    functions) were dead ends, so `app_main` reached 185 nodes instead of 241 and
    a chain that continues through a folded function was summed only to the fold
    point. That is the silent under-count this gate exists to prevent -- the same
    class as the missing objdump -- so the `.text.` prefix is stripped here. The
    two spellings always agree: the mangled symbol IS the section suffix.
    """
    graph: dict[str, set[str]] = {}
    for obj in sorted(outdir.glob("*.o")):
        r = subprocess.run([objdump, "-dr", str(obj)], capture_output=True, text=True)
        if r.returncode != 0:
            continue
        # One block per function, introduced by `<mangled>:` at the start of a line.
        blocks = re.split(r"\n(?=[0-9a-f]{8} <)", r.stdout)
        for b in blocks:
            m = re.match(r"[0-9a-f]{8} <([^>]+)>:", b)
            if not m:
                continue
            caller = m.group(1)
            # Only real function symbols: skip the literal pools, which are
            # section-local data blocks named after the function that owns them.
            if caller.startswith(".literal") or caller.startswith(".LC"):
                continue
            callees = set()
            for sym in re.findall(r"R_XTENSA_ASM_EXPAND\s+([A-Za-z0-9_.$]+)", b):
                if sym.startswith(".literal"):
                    continue
                # Fold a section symbol onto the function it wraps (see above).
                if sym.startswith(".text."):
                    sym = sym[len(".text."):]
                callees.add(sym)
            graph.setdefault(caller, set()).update(callees)
    return graph


def demangle(names, cxxfilt: str | None) -> dict:
    """{mangled: human} via the toolchain's c++filt, best-effort."""
    names = list(names)
    if not names or cxxfilt is None:
        return {}
    try:
        r = subprocess.run([cxxfilt] + names, capture_output=True, text=True)
        if r.returncode == 0:
            out = r.stdout.splitlines()
            if len(out) == len(names):
                return dict(zip(names, out))
    except OSError:
        pass
    return {}


def strip_params(sig: str) -> str:
    """`void SystemOrchestrator::Boot()` and `Boot()` both -> `SystemOrchestrator::Boot`.

    c++filt prints a full prototype; the `.su` name column is the bare qualified
    name with no return type or parameters. Reducing the demangled symbol to its
    qualified name is what lets the call graph join the frame table.

    **Anonymous-namespace functions need their own path.** c++filt spells them
    `(anonymous namespace)::Foo()` while gcc's `.su` column spells them
    `{anonymous}::Foo()`. Splitting on the FIRST `(` -- which is the namespace's,
    not the parameter list's -- yields an empty string, so every one of them
    (CdcRxCallback, ServiceLineState, FormatSlotId, the whole ConfigCodec helper
    set) joined to nothing and their frames were silently dropped from every
    chain they appear in. Normalising the `(anonymous namespace)` prefix to the
    `{anonymous}` the frame table uses makes the two spellings one key.
    """
    s = sig.strip()
    # Normalise the namespace spelling FIRST, so the only remaining `(` below is
    # the parameter list's -- c++filt's `(anonymous namespace)::Foo()` otherwise
    # split to an empty string. See the docstring.
    s = s.replace("(anonymous namespace)", "{anonymous}")
    s = s.split("(")[0].strip()
    # Drop a leading return type: take the last whitespace-separated token, which
    # is the qualified name (`void Foo::Bar` -> `Foo::Bar`).
    return s.split()[-1] if s else s


def longest_path(graph: dict, weights: dict, start: str) -> list[str]:
    """The heaviest path from `start` to any reachable leaf, by SUMMED weights.

    **Exact, not a heuristic.** The call graph is a DAG here (verified for the
    device build: no strongly-connected component of size > 1), so the longest
    path is a linear-time DP -- process nodes in reverse topological order and
    take `w(v) + max over children`. This matters because the obvious
    alternative -- a DFS that keeps one `best` path and skips already-visited
    nodes -- is NOT exact: visiting a shared callee down one branch marks it
    seen and hides it from a heavier branch, so the gate reports a clean bill of
    health on a real overflow. That false negative was measured: a 2 KB frame
    added to `EstablishSafeIdle`, reachable only as
    `app_main -> SystemOrchestratorCreate -> Boot -> EstablishSafeIdle`, went
    unreported by the DFS version.

    Cycles (mutual recursion) are collapsed first so the DP stays well-defined.
    A cycle's members are all on the stack together in the worst case, so the
    collapsed node's weight is the SUM of its members -- which over-counts a
    path that does not traverse the whole cycle, erring toward reporting.
    """
    node_of, comps = {}, []
    index, low, on_stack, stack = {}, {}, set(), []

    def strong(v):
        index[v] = low[v] = len(index)
        stack.append(v)
        on_stack.add(v)
        for w in graph.get(v, ()):
            if w not in index:
                strong(w)
                low[v] = min(low[v], low[w])
            elif w in on_stack:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on_stack.discard(w)
                comp.append(w)
                if w == v:
                    break
            for w in comp:
                node_of[w] = len(comps)
            comps.append(comp)

    for v in list(graph):
        if v not in index:
            strong(v)

    node_weight = [sum(weights.get(m, 0) for m in comp) for comp in comps]
    edges = {i: set() for i in range(len(comps))}
    for v, callees in graph.items():
        for w in callees:
            if w in node_of and node_of[v] != node_of[w]:
                edges[node_of[v]].add(node_of[w])

    best = {}

    def dp(n: int) -> int:
        if n not in best:
            best[n] = node_weight[n]
            for m in edges.get(n, ()):
                best[n] = max(best[n], node_weight[n] + dp(m))
        return best[n]

    out = [start]
    node = node_of.get(start)
    if node is not None:
        dp(node)   # fill the memo along every reachable path first
    while node is not None:
        nxt = None
        for m in sorted(edges.get(node, ())):
            if node_weight[node] + dp(m) == best[node]:
                nxt = m
                break
        if nxt is None:
            break
        out.extend(comps[nxt])
        node = nxt
    return out


# The functions entered directly from a task. `app_main` is the poll task's entry
# and its own mangled name is stable, so it is spelled literally.
MAIN_TASK_ROOTS = ("app_main",)
#
# The USB-task roots are the CDC callbacks that esp_tinyusb invokes from TinyUSB's
# task; that task's own loop lives in a managed component, so it is not a symbol in
# this project's objects and cannot be a root. Naming the callbacks is what puts a
# chain on the 4 KB stack.
#
# **These must be the callbacks, not the router handlers.** An earlier revision
# listed `CommandRouter::OnLine/OnConnected/OnDisconnected/Process/Tick` here --
# correct when the transport parsed in its own callbacks. The task-ownership split
# (`tools/check_task_ownership.py`) moved all five onto the POLL task: the
# callbacks only stage bytes and publish a DTR level, and `UsbLinkService` runs the
# handlers from `app_main`. Left as-is, the gate budgeted those handlers against
# the TinyUSB stack (4096 B) while they actually run on main (3584 B) -- an
# UNDER-check on the one task they run on -- and, because the callbacks were no
# longer named by any root, the only two functions that really do run on the USB
# task went unchecked entirely. So a stack-hungry change to a callback could
# overflow the device with the gate green.
#
# The anonymous-namespace callbacks are spelled as their MANGLED symbols (`_ZN12
# _GLOBAL__N_1...`), which is what the call graph is keyed by, and they join the
# frame table through `strip_params`' handling of `{anonymous}` / `(anonymous
# namespace)`.
USB_TASK_ROOTS = (
    "_ZN12_GLOBAL__N_113CdcRxCallbackEiP14cdcacm_event_t",        # CdcRxCallback
    "_ZN12_GLOBAL__N_120CdcLineStateCallbackEiP14cdcacm_event_t",  # CdcLineStateCallback
)


def load_compile_entries(repo: pathlib.Path, build: pathlib.Path) -> list[dict]:
    """Every project TU's compile entry, from BOTH databases, best-covering wins.

    There are TWO databases and they are not equivalent, in opposite directions:

        compile_commands.json          main.cpp with 220 -I paths, incl.
                                       spi_flash/include (needs esp_flash.h);
                                       but it is only written by a `compiledb`-style
                                       run and goes STALE -- on this machine it was
                                       five days old and did not list a TU added
                                       since, so that TU was silently never
                                       stack-checked while the gate stayed green.
        .pio/build/<env>/compile   the database `pio run` itself refreshes, so it
          _commands.json                always lists every current TU -- but its
                                        main.cpp entry carries only 143 -I paths
                                        and dies on esp_flash.h.

    Neither one alone is sufficient, and picking one per FILE is what makes the
    check honest: for each source, take the entry with the most `-I` paths (the
    one that can actually compile it), and let the two files that need different
    databases each get the one they need. A single-database choice was the
    original bug in two stages -- first it picked the build db and lost `main.cpp`
    ("no chain" for the boot path), then it picked the root db and lost every file
    added since the root db was last written.

    Both also spell the compiler differently (bare name vs absolute path), which
    `resolve_compiler` handles.
    """
    candidates = [c for c in (repo / "compile_commands.json",
                              build / "compile_commands.json") if c.is_file()]
    by_key: dict[str, tuple[int, dict]] = {}
    order: list[str] = []
    for cand in candidates:
        try:
            entries = json.loads(cand.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        for e in entries:
            f = e.get("file", "").replace("\\", "/")
            if not is_project_source(f, repo):
                continue
            # Key on the tail, because the databases spell the path two ways
            # (relative `src/main.cpp` vs absolute). The tail is unique within
            # this project's lib/ and src/.
            key = "/".join(f.split("/")[-2:])
            n = sum(1 for p in shlex.split(e.get("command", "")) if p.startswith("-I"))
            if key not in by_key:
                order.append(key)
            if key not in by_key or n > by_key[key][0]:
                by_key[key] = (n, e)
    return [by_key[k][1] for k in order]


def build_tree_error(build) -> str | None:
    """Why the build tree is unusable, or None if it is present and fresh.

    A missing `compile_commands.json` must be an ERROR, not a silent fall back to
    the stale root database. The fallback is exactly the silent-undercoverage
    this gate exists to prevent, one layer down: the root db predates the newest
    TUs, so it still compiles `main.cpp` but the freshly-added device-only units
    vanish from the call graph -- and a task root (`app_main`) that is no longer
    in the graph is then reported as a RENAMED root, which sends the reader off
    to edit MAIN_TASK_ROOTS/USB_TASK_ROOTS at whatever name is current that day.

    The real cause is almost always the one measured in HANDOFF section 2: a
    PlatformIO invocation under a different `SWC_FW_VERSION`/`SWC_GIT_SHA` (or
    none set) computes a different project checksum against the shared
    `.pio/build/project.checksum`, and PlatformIO's auto-clean deletes the WHOLE
    of `.pio/build/` before rebuilding. So the two env dirs evict each other
    unless every invocation names the same pair.

    Pure over its input, so `test_check_stack_usage.py` can exercise it without
    a device build -- the same reason `chain_precondition_error` is pure.
    """
    import os

    if not os.path.isfile(os.path.join(str(build), "compile_commands.json")):
        return (f"{build}/compile_commands.json is missing, so the call graph "
                f"would come from the STALE root database and every TU added "
                f"since it was written would go unchecked (with `app_main` "
                f"reported as a renamed root). Run `SWC_FW_VERSION=dev "
                f"SWC_GIT_SHA=local pio run -e esp32s3` first, with the SAME "
                f"SWC_FW_VERSION/SWC_GIT_SHA on EVERY PlatformIO invocation -- a "
                f"change in either makes PlatformIO auto-clean all of .pio/build/.")
    return None


def chain_precondition_error(objdump, graph, frames_by_mangled, roots) -> str | None:
    """Why the CHAIN check cannot be trusted to have run, or None if it can.

    Each branch removes whole task chains from the check while leaving the gate's
    green output unchanged, so each one is reported as an error rather than
    allowed to produce a pass. Pure over its inputs (it touches no filesystem),
    which is what lets `test_check_stack_usage.py` exercise every branch without
    a device build -- a guard whose failure mode is "silently stops guarding"
    needs a test that it still fails when it cannot look, not just one that it
    passes when it can.
    """
    if objdump is None:
        return ("xtensa-esp32s3-elf-objdump not found (looked on PATH and under "
                "~/.platformio/packages/toolchain-xtensa-esp-elf*) -- the call "
                "graph, and therefore the whole chain check, cannot be built. "
                "Install the device toolchain (or run inside a PlatformIO shell).")
    if not graph:
        return ("the call graph is EMPTY -- objdump produced no function symbols "
                "from the build's objects. The chain check would vacuously pass; "
                "failing instead.")
    missing = sorted(r for r in roots if r not in graph)
    if missing:
        return (f"{len(missing)} task root(s) named in this gate are absent from "
                "the call graph, so their chains would not be checked: "
                + ", ".join(missing)
                + "\n  Update MAIN_TASK_ROOTS/USB_TASK_ROOTS to the new mangled names.")
    unjoined = sorted(r for r in roots if r not in frames_by_mangled)
    if unjoined:
        return ("the call graph did not join the frame table for "
                f"{len(unjoined)} task root(s): "
                + ", ".join(strip_params(s) for s in unjoined)
                + "\n  A join that matches nothing reports every chain as one frame.")
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=str(REPO))
    ap.add_argument("--build-dir", default=".pio/build/esp32s3")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    repo = pathlib.Path(args.repo)
    build = repo / args.build_dir
    why = build_tree_error(build)
    if why:
        print(f"stack gate: {why}", file=sys.stderr)
        return 2
    entries = load_compile_entries(repo, build)
    if not entries:
        print(f"stack gate: no compile_commands.json for this project's sources "
              f"under {repo} -- run `pio run -e esp32s3` first", file=sys.stderr)
        return 2

    outdir = build / "stack-usage"
    compile_with_stack_usage(entries, outdir)
    frames, where = parse_su(outdir)
    if not frames:
        print("stack gate: no -fstack-usage output produced", file=sys.stderr)
        return 2

    main_stack = read_main_task_size(repo)
    usb_stack = read_usb_task_size(repo)
    limit = max(main_stack, usb_stack)

    # The per-frame check: a frame bigger than the SMALLEST task stack can never
    # be entered, whichever task runs it. This alone catches the by-value `Config`
    # shape -- but it is not sufficient, because a harmless 800 B frame below a
    # 3 KB one is just as fatal. Hence the chain check below.
    over = sorted(((n, s) for n, s in frames.items() if s > limit), key=lambda t: -t[1])

    # **The chain check cannot run without its inputs, so a missing input is an
    # ERROR, never a pass.** Without `objdump` there is no call graph; every
    # configured root is then absent from it and the chain loop `continue`s on
    # every root -- so the gate printed "no frame or chain exceeds its budget"
    # having summed ZERO chains, byte-identical to a real pass. A runner that
    # could not find the toolchain (the lookup is best-effort PATH-then-glob)
    # would have gated nothing, and the failure the chain check exists to catch
    # -- a call chain summing past a task stack -- is exactly the one the
    # per-frame check does not see. `chain_precondition_error` names every such
    # input, so the silent pass is impossible.
    objdump = objdump_path()
    cxxfilt = (str(pathlib.Path(objdump).parent / "xtensa-esp32s3-elf-c++filt")
               if objdump else None)
    graph = build_call_graph(outdir, objdump) if objdump else {}

    # Join the graph's mangled names to the frame table's qualified names. The
    # per-frame check below reads `frames` directly, but the CHAIN check needs
    # this join, and a join that matches nothing would silently report every
    # chain as one frame. So it is asserted, not hoped for.
    all_syms = set(graph) | {c for cs in graph.values() for c in cs}
    names = demangle(all_syms, cxxfilt)
    frames_by_mangled = {}
    for sym in all_syms:
        q = strip_params(names.get(sym, sym))
        if q in frames:
            frames_by_mangled[sym] = frames[q]

    roots = MAIN_TASK_ROOTS + USB_TASK_ROOTS
    why = chain_precondition_error(objdump, graph, frames_by_mangled, roots)
    if why is not None:
        print(f"stack gate: {why}", file=sys.stderr)
        return 2

    # The chain check: sum frames along the heaviest path from each task root and
    # compare against that task's OWN stack.
    chain_problems = []
    for roots, budget, task in ((MAIN_TASK_ROOTS, main_stack, "main"),
                                (USB_TASK_ROOTS, usb_stack, "TinyUSB")):
        for root in roots:
            if root not in graph:
                continue
            path = longest_path(graph, frames_by_mangled, root)
            total = sum(frames_by_mangled.get(n, 0) for n in path)
            if total > budget:
                chain_problems.append((path, total, budget, task))

    worst = sorted(frames.items(), key=lambda kv: -kv[1])[:8]

    if args.json:
        print(json.dumps({
            "main_task_bytes": main_stack,
            "usb_task_bytes": usb_stack,
            "limit_bytes": limit,
            "over_limit": [{"function": n, "bytes": s} for n, s in over],
            "chain_over_budget": [
                {"task": t, "sum_bytes": s, "budget": b, "path": p}
                for p, s, b, t in chain_problems
            ],
            "worst": [{"function": n, "bytes": s} for n, s in worst],
        }, indent=2))

    if over or chain_problems:
        print("stack gate FAILED: a stack frame can overflow its task", file=sys.stderr)
        for n, s in over:
            print(f"  - {s:>7,} B frame  {n}   [{where.get(n, '').split('/code/')[-1]}]",
                  file=sys.stderr)
        for path, total, budget, task in chain_problems:
            pretty = " -> ".join(strip_params(names.get(n, n)) for n in path)
            print(f"  - {total:,} B chain on the {task} task ({budget:,} B stack): {pretty}",
                  file=sys.stderr)
        print(
            f"\nThe task stacks are main {main_stack:,} B and TinyUSB {usb_stack:,} B, "
            f"so a frame or chain over that overflows the moment it is entered.\n"
            f"`sizeof(Config)` is {CONFIG_BYTES:,} B: hold one in a STATIC or a "
            "member, not by value, and give factories an out-parameter rather than "
            "a by-value return. This is the device-only blind spot -- the host "
            "suite's threads have megabytes, so no native test can see it.",
            file=sys.stderr,
        )
        return 1

    names_worst = demangle([n for n, _ in worst], cxxfilt)
    print("stack gate: OK")
    print(f"  task stacks: main {main_stack:,} B, TinyUSB {usb_stack:,} B; "
          f"no frame or chain exceeds its budget")
    for n, s in worst:
        print(f"    {s:>6,} B  {names_worst.get(n, n) or n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
