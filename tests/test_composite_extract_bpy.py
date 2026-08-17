"""Integration test: COMPOSITS extraction over the real golden scene."""

import math
import sys
from pathlib import Path

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))
sys.path.insert(0, str(REPO_ROOT / "tools"))

from golden_uids import UIDS  # noqa: E402
from mh4blend.core.canonical import P_ROTATION_QUAT, P_SCALE, P_TRANSLATION_CM  # noqa: E402
from mh4blend.scene.composite_extract import extract_composites  # noqa: E402

GOLDEN = REPO_ROOT / "golden" / "golden.blend"
MUTATIONS = REPO_ROOT / "golden" / "mutations"

pytestmark = pytest.mark.skipif(
    not GOLDEN.exists(), reason="golden.blend not generated (run tools/run_all.py)")


def extract(path):
    bpy.ops.wm.open_mainfile(filepath=str(path))
    composites = extract_composites(bpy.data.scenes["COMPOSITS"])
    return {c.uid: c for c in composites}


def test_golden_structure():
    composites = extract(GOLDEN)
    assert set(composites) == {UIDS["col/ca_windowset"], UIDS["col/ca_building"]}

    ws = composites[UIDS["col/ca_windowset"]]
    nodes = {n.node_uid: n for n in ws.nodes}
    assert len(nodes) == 3
    w3 = nodes[UIDS["node/ca_windowset/window_3"]]
    assert w3.kind == "mesh"
    assert w3.resource_uid == UIDS["col/window_a"]
    # location (3.0, 0.2, 1.1) m -> UE (300, -20, 110) cm, quantized p=3
    assert w3.local_transform.translation == (300000, -20000, 110000)
    assert w3.local_transform.scale == (1200000,) * 3
    # 15 deg around Z in Blender -> §11-mapped quat, sign-canonical
    half = math.radians(15.0) / 2.0
    expected_z = round(-math.sin(half) * 10 ** P_ROTATION_QUAT)
    assert abs(w3.local_transform.rotation[2] - expected_z) <= 1

    cb = composites[UIDS["col/ca_building"]]
    nodes = {n.node_uid: n for n in cb.nodes}
    assert len(nodes) == 8
    assert nodes[UIDS["node/ca_building/windows_front"]].kind == "composite_ref"
    assert (nodes[UIDS["node/ca_building/windows_front"]].resource_uid
            == UIDS["col/ca_windowset"])
    group = nodes[UIDS["node/ca_building/props_group"]]
    assert group.kind == "group" and group.resource_uid is None
    for child_key in ("gp_wall", "gp_decal"):
        child = nodes[UIDS[f"node/ca_building/{child_key}"]]
        assert child.parent_uid == UIDS["node/ca_building/props_group"]
    # gp_wall local translation is parent-relative: (0.5, 0, 0) m
    gp_wall = nodes[UIDS["node/ca_building/gp_wall"]]
    assert gp_wall.local_transform.translation == (50000, 0, 0)


def test_decal_role_inherited_from_resource_collection():
    composites = extract(GOLDEN)
    cb = composites[UIDS["col/ca_building"]]
    leak = next(n for n in cb.nodes
                if n.node_uid == UIDS["node/ca_building/leak_decal"])
    assert leak.properties == {"role": "decal"}  # TODO(QUESTION-9) semantics
    wall = next(n for n in cb.nodes
                if n.node_uid == UIDS["node/ca_building/wall_main"])
    assert wall.properties == {}


def test_reparent_mutation_extracts_new_parent_and_transform():
    base = extract(GOLDEN)[UIDS["col/ca_building"]]
    mutated = extract(MUTATIONS / "reparent_node.blend")[UIDS["col/ca_building"]]
    before = {n.node_uid: n for n in base.nodes}[UIDS["node/ca_building/leak_decal"]]
    after = {n.node_uid: n for n in mutated.nodes}[UIDS["node/ca_building/leak_decal"]]
    assert before.parent_uid is None
    assert after.parent_uid == UIDS["node/ca_building/props_group"]
    assert after.local_transform != before.local_transform


def test_dangling_parent_mutation_raises_at_extraction():
    bpy.ops.wm.open_mainfile(
        filepath=str(MUTATIONS / "parent_uid_dangling.blend"))
    with pytest.raises(ValueError, match="MH_E_DANGLING_PARENT"):
        extract_composites(bpy.data.scenes["COMPOSITS"])
