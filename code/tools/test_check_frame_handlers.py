"""Tests for check_frame_handlers -- the guard that would have caught `link_gap`.

The guard's whole value is that it FAILS on the defect it names, so these tests
drive it against deliberately damaged copies of the app sources rather than
against the tree, which happens to be clean. A guard tested only on a passing
tree is a guard nobody has seen work.
"""

import os
import re
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
CODE = os.path.dirname(HERE)
GUARD = os.path.join(HERE, "check_frame_handlers.py")


def _run(env_extra=None):
    env = dict(os.environ)
    env.update(env_extra or {})
    return subprocess.run([sys.executable, GUARD], capture_output=True, text=True, env=env)


def test_it_passes_on_the_tree():
    r = _run()
    assert r.returncode == 0, r.stdout + r.stderr
    assert "OK" in r.stdout


def test_it_fails_when_a_frame_constant_is_not_referenced(tmp_path):
    # The exact `link_gap` defect: the frame is in the vocabulary, the constant is
    # generated, and the app handles nothing. Simulated by pointing the guard at a
    # copy of the app tree with the one reference renamed.
    app = tmp_path / "app"
    shutil.copytree(os.path.join(CODE, "android", "app", "src", "main", "java",
                                 "com", "oetsolutions", "swc"), app)
    target = app / "app" / "AppViewModel.kt"
    s = target.read_text()
    assert "Frames.LINK_GAP" in s
    target.write_text(s.replace("Frames.LINK_GAP", "Frames.LINK_GAP_MUTANT"))

    # The guard resolves the tree from its own location, so run a copy of it
    # beside the damaged tree with `APP` rewritten -- and beside a copy of the
    # schema, which it imports from its own directory.
    shutil.copy(os.path.join(HERE, "contract_schema.py"), tmp_path / "contract_schema.py")
    guard = tmp_path / "check.py"
    gsrc = open(GUARD, encoding="utf-8").read()
    gsrc = gsrc.replace(
        'APP = os.path.join(CODE, "android", "app", "src", "main", "java", "com",\n                   "oetsolutions", "swc")',
        'APP = %r' % str(app))
    guard.write_text(gsrc)
    r = subprocess.run([sys.executable, str(guard)], capture_output=True, text=True)
    assert r.returncode == 1, "the guard must fail on an unhandled frame\n" + r.stdout
    assert "link_gap" in r.stdout
    assert "no app-side handler" in r.stdout


def test_the_schema_directions_are_all_known():
    # The guard reads the schema, so this pins the SCHEMA side: if a frame is
    # added with a direction the guard does not expect, this fails rather than the
    # guard silently checking fewer frames than exist.
    sys.path.insert(0, HERE)
    import contract_schema

    known = {"fw2app", "both", "app2fw"}
    for f in contract_schema.FRAMES:
        assert f.direction in known, f"{f.name} has an unexpected direction {f.direction}"
    fw2app = [f.name for f in contract_schema.FRAMES if f.direction in ("fw2app", "both")]
    # `hello` and `event` are the two the app must handle for the link to work at
    # all; a guard that stopped covering them would be vacuous.
    assert "hello" in fw2app and "event" in fw2app and "status" in fw2app
