"""Stage-A acceptance runner: regenerate everything headless in one command.

    python3 tools/run_all.py               # pip `bpy` module (Blender 4.5+)
    MH_BLENDER=/path/to/blender python3 tools/run_all.py   # blender -b -P

Steps: golden scene -> all 7 mutations -> regenerate expected-diff
markdown -> pytest. Any failure stops the run with a non-zero exit code.
"""

import os
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)

MUTATIONS = (
    "rename_object", "rename_collection", "linked_duplicate",
    "make_single_user", "delete_node", "edit_geometry", "reparent_node",
    # Negative scenarios (QUESTION-7): scenes that must FAIL stage-B export
    # validation; their spec lives in golden/expected_errors/.
    "duplicate_uid", "parent_uid_dangling",
)


def run_blender_script(script_path):
    blender = os.environ.get("MH_BLENDER")
    if blender:
        cmd = [blender, "-b", "--factory-startup", "--python-exit-code", "1",
               "-P", script_path]
    else:
        cmd = [sys.executable, script_path]
    shown = [os.path.basename(cmd[0])] + [
        os.path.relpath(p, REPO_ROOT) if os.path.isabs(p) else p for p in cmd[1:]]
    print(f"--- {' '.join(shown)}")
    subprocess.run(cmd, check=True, cwd=REPO_ROOT)


def main():
    run_blender_script(os.path.join(TOOLS_DIR, "make_golden_scene.py"))
    for name in MUTATIONS:
        run_blender_script(os.path.join(TOOLS_DIR, "mutations", f"{name}.py"))
    # The composite-cycle negative fixture is bundle-level JSON, not a .blend:
    # Blender cannot persist an instance_collection cycle (see the script).
    subprocess.run(
        [sys.executable, os.path.join(TOOLS_DIR, "make_cycle_fixture.py")],
        check=True, cwd=REPO_ROOT)
    subprocess.run(
        [sys.executable, os.path.join(TOOLS_DIR, "render_expected_diffs.py")],
        check=True, cwd=REPO_ROOT)
    subprocess.run(
        [sys.executable, "-m", "pytest", "tests", "-q"],
        check=True, cwd=REPO_ROOT)
    print("stage A pipeline: all steps passed")


if __name__ == "__main__":
    main()
