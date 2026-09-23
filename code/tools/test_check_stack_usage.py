"""The stack gate must FAIL when it cannot run, not pass.

The gate's chain check needs three inputs -- objdump, a non-empty call graph, and
a frame-table join for every configured task root. Losing any one of them makes
the check sum ZERO chains while printing the SAME green line it prints after a
real pass, so a reviewer reading the output cannot tell the difference. The gate
then reports "no frame or chain exceeds its budget" about a chain check that
never ran, and the failure it exists to catch (a call chain summing past a task
stack -- invisible to the per-frame check) sails through.

These tests exercise the precondition directly, without needing a device build,
because that silent-pass behaviour is the whole reason the function exists.
"""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import check_stack_usage as cs  # noqa: E402


def _real_graph():
    objdump = cs.objdump_path()
    if objdump is None:
        return None, None
    outdir = pathlib.Path(".pio/build/esp32s3/stack-usage")
    return objdump, cs.build_call_graph(outdir, objdump)


def test_no_objdump_is_an_error():
    # The exact shape that used to pass: no toolchain -> `graph = {}` -> every
    # root absent -> chain loop skipped -> "OK".
    why = cs.chain_precondition_error(None, {}, {}, cs.MAIN_TASK_ROOTS)
    assert why is not None
    assert "objdump" in why


def test_empty_graph_is_an_error():
    why = cs.chain_precondition_error("/x/objdump", {}, {}, cs.MAIN_TASK_ROOTS)
    assert why is not None
    assert "EMPTY" in why


def test_a_renamed_root_is_an_error():
    # A root that moved out of the build silently drops that task's chain from the
    # check; it must be named here rather than discovered by a device crash.
    roots = cs.MAIN_TASK_ROOTS + cs.USB_TASK_ROOTS
    graph = {"app_main": set()}
    why = cs.chain_precondition_error("/x/objdump", graph, {"app_main": 32}, roots)
    assert why is not None
    assert "absent from" in why


def test_an_unjoined_root_is_an_error():
    # Present in the graph but not joined to the frame table: the chain would sum
    # as one frame and report a false clean bill.
    roots = cs.MAIN_TASK_ROOTS + cs.USB_TASK_ROOTS
    graph = {r: set() for r in roots}
    why = cs.chain_precondition_error("/x/objdump", graph, {}, roots)
    assert why is not None
    assert "join" in why


def test_a_healthy_gate_has_no_precondition_error():
    # Mutation guard in the other direction: a gate that reported an error for a
    # GOOD tree would be turned off by whoever hits it.
    objdump, graph = _real_graph()
    if objdump is None or not graph:
        return  # no device build on this machine; the branches above still ran
    roots = cs.MAIN_TASK_ROOTS + cs.USB_TASK_ROOTS
    frames_by_mangled = {r: 32 for r in roots}
    assert cs.chain_precondition_error(objdump, graph, frames_by_mangled, roots) is None


def test_strip_params_normalises_anonymous_namespace():
    # c++filt spells it `(anonymous namespace)::Foo()`, the `.su` column spells it
    # `{anonymous}::Foo`. They must reduce to ONE key, or every anonymous-namespace
    # function joins to nothing and its frame is dropped from every chain.
    assert cs.strip_params("(anonymous namespace)::CdcRxCallback(int, cdcacm_event_t*)") \
        == "{anonymous}::CdcRxCallback"
    assert cs.strip_params("{anonymous}::CdcRxCallback") == "{anonymous}::CdcRxCallback"


def test_strip_params_keeps_a_qualified_cxx_name():
    assert cs.strip_params("void SystemOrchestrator::Boot()") == "SystemOrchestrator::Boot"
    assert cs.strip_params("CommandRouter::Nack(unsigned long, char const*, char const*)") \
        == "CommandRouter::Nack"


def test_the_usb_roots_are_the_callbacks_not_the_router_handlers():
    # The task-ownership split moved every CommandRouter handler onto the POLL
    # task; budgeting them against the TinyUSB stack under-checked the task they
    # run on, and left the real USB-task functions unchecked. Pin the intent: the
    # roots name the CDC callbacks, and no longer the router.
    roots = " ".join(cs.USB_TASK_ROOTS)
    assert "CdcRxCallback" in roots
    assert "CdcLineStateCallback" in roots
    assert "CommandRouter" not in roots


def test_no_section_symbol_edge_survives_in_the_real_graph():
    # A `.text.<mangled>` section symbol is not a graph node, so an edge to one is
    # a dead end that truncates the chain through it (129 such edges before the
    # fold). The real graph must have none.
    objdump, graph = _real_graph()
    if objdump is None or not graph:
        return
    phantoms = [v for vs in graph.values() for v in vs if v.startswith(".text.")]
    assert phantoms == []


def test_app_main_reaches_the_router_handlers_that_run_on_the_poll_task():
    # Guards the coverage the roots fix could have silently dropped: OnConnected is
    # reached only through ServiceLineState, which is only reachable once the
    # `.text.` fold is in place. If a future change makes it unreachable again, the
    # chain check stops covering it while still reporting green.
    objdump, graph = _real_graph()
    if objdump is None or not graph:
        return
    seen, stack = set(), ["app_main"]
    while stack:
        n = stack.pop()
        if n in seen:
            continue
        seen.add(n)
        stack.extend(graph.get(n, ()))
    assert any("OnConnected" in s for s in seen)


def test_config_bytes_is_read_from_the_model_assertion_not_a_second_copy():
    # The constant used to CLAIM it was "asserted against the model below by the
    # build rather than trusted" while being a hand-written 8912 that nothing
    # asserted -- the failure message could quote a size the model no longer had.
    # Pin the real invariant: the gate's number IS the model's asserted number, and
    # the read is strict, so deleting the assertion fails rather than silently
    # reverting to a remembered value.
    assert cs.CONFIG_BYTES == 8912
    src = (cs.REPO / "lib" / "Config" / "ConfigModel.h").read_text()
    m = __import__("re").search(r"static_assert\(\s*sizeof\(Config\)\s*==\s*(\d+)", src)
    assert m is not None, "ConfigModel.h must pin sizeof(Config) with a static_assert"
    assert int(m.group(1)) == cs.CONFIG_BYTES


def test_a_missing_config_assertion_is_an_error_not_a_fallback():
    # The strict-read contract, exercised against a tree that lacks the assertion.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        (root / "lib" / "Config").mkdir(parents=True)
        (root / "lib" / "Config" / "ConfigModel.h").write_text("struct Config {};\n")
        try:
            cs._read_config_bytes(root)
        except RuntimeError as e:
            assert "static_assert" in str(e)
        else:
            raise AssertionError("a missing assertion must raise, not fall back")



def test_the_two_compile_databases_are_merged_per_file():
    # The defect this pins: the gate used to pick ONE database. Picking the build
    # db lost `src/main.cpp` ("no chain" for the boot path); picking the root db
    # lost every TU added after it was last written, so a brand-new device-only
    # file was NEVER stack-checked while the gate stayed green -- exactly the
    # silent-undercoverage the module docstring is about, one layer down.
    #
    # The merge must keep BOTH: main.cpp (which only the root db can compile) and
    # a TU that only the build db lists.
    import tempfile, json
    with tempfile.TemporaryDirectory() as td:
        repo = pathlib.Path(td)
        (repo / "src").mkdir(parents=True)
        (repo / "lib" / "New").mkdir(parents=True)
        (repo / "src" / "main.cpp").write_text("int main(){return 0;}")
        (repo / "lib" / "New" / "Fresh.cpp").write_text("int f(){return 1;}")

        # Root db: has main.cpp (many includes), predates Fresh.cpp.
        (repo / "compile_commands.json").write_text(json.dumps([
            {"file": "src/main.cpp", "directory": str(repo),
             "command": "cc -I" + " -I".join(["a"] * 40) + " -c src/main.cpp"},
        ]))
        # Build db: fresh, lists Fresh.cpp with fewer includes on main.cpp.
        build = repo / ".pio" / "build" / "esp32s3"
        build.mkdir(parents=True)
        (build / "compile_commands.json").write_text(json.dumps([
            {"file": str(repo / "src" / "main.cpp"), "directory": str(repo),
             "command": "cc -I" + " -I".join(["b"] * 10) + " -c src/main.cpp"},
            {"file": str(repo / "lib" / "New" / "Fresh.cpp"), "directory": str(repo),
             "command": "cc -Ix -c lib/New/Fresh.cpp"},
        ]))

        entries = cs.load_compile_entries(repo, build)
        names = sorted(pathlib.Path(e["file"]).name for e in entries)
        assert names == ["Fresh.cpp", "main.cpp"], names

        # And main.cpp got the RICHER entry (the root db's, 40 includes).
        main_entry = next(e for e in entries if e["file"].endswith("main.cpp"))
        assert sum(1 for p in main_entry["command"].split() if p.startswith("-I")) == 40


def test_a_missing_build_tree_is_an_error_not_a_stale_db_fallback():
    # The defect this pins: when `.pio/build/<env>/compile_commands.json` is gone,
    # `load_compile_entries` silently falls back to the ROOT database. Its
    # `main.cpp` entry still compiles, so the gate went green -- while every TU
    # added after that file was last written (including a whole device-only
    # unit) was missing from the call graph. The visible symptom was a task root
    # reported as RENAMED, which reads as a source drift and is not one.
    #
    # The realistic cause is PlatformIO's auto-clean: it keys on ONE
    # `.pio/build/project.checksum` shared by every env, and `platformio.ini`
    # bakes `SWC_FW_VERSION`/`SWC_GIT_SHA` (from the environment) into the
    # checksummed config. So a `pio test -e native` after a dev-var'd `pio run
    # -e esp32s3` wipes the device tree. The gate must say THAT, not "update
    # MAIN_TASK_ROOTS to the new mangled names".
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        build = pathlib.Path(td) / ".pio" / "build" / "esp32s3"
        build.mkdir(parents=True)
        why = cs.build_tree_error(build)
        assert why is not None, "a missing build tree must be an error"
        assert "compile_commands.json" in why
        assert "pio run -e esp32s3" in why
        # It must name the two env vars, because they are what triggers the
        # auto-clean that empties the tree in the first place.
        assert "SWC_FW_VERSION" in why and "SWC_GIT_SHA" in why
        # And it must NOT send the reader off to edit the root name list.
        assert "MAIN_TASK_ROOTS" not in why

        (build / "compile_commands.json").write_text("[]")
        assert cs.build_tree_error(build) is None, "a present tree is not an error"

