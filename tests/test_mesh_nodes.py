"""Pure node-classifier gates for the v4 mesh FBX dialect."""

from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.mesh_nodes import (  # noqa: E402
    MeshNode,
    build_mesh_import_plan,
    validate_node_markers,
)
from mh4blend.core.validate import MHValidationError  # noqa: E402


@pytest.mark.parametrize(
    ("name", "node_type", "kind", "level", "mode"),
    [
        ("body", "MESH", "render", 0, None),
        ("body_lod03", "MESH", "render", 3, None),
        ("UCX_body", "MESH", "collision", None, "both"),
        ("body_cls_phys", "MESH", "collision", None, "phys"),
        ("body_cls_trace", "MESH", "collision", None, "trace"),
        ("SOCKET_grip", "NULL", "socket", None, None),
        ("pivot", "EMPTY", "group", None, None),
    ],
)
def test_classifier_returns_frozen_transport_semantics(
        name, node_type, kind, level, mode):
    result = validate_node_markers(name, node_type)
    assert (result.kind, result.lod_level, result.collision_mode) == (
        kind, level, mode)


@pytest.mark.parametrize(
    ("name", "node_type", "has_children"),
    [
        ("UCX_body_cls_both", "MESH", False),
        ("UCX_body_lod00", "MESH", False),
        ("SOCKET_grip", "MESH", False),
        ("UCX_body", "NULL", False),
        ("body_cls_phys", "NULL", False),
        ("pivot_lod00", "NULL", False),
        ("SOCKET_grip", "NULL", True),
    ],
)
def test_all_invalid_marker_combinations_share_one_code(
        name, node_type, has_children):
    with pytest.raises(MHValidationError) as exc:
        validate_node_markers(name, node_type, has_children=has_children)
    assert exc.value.code == "MH_E_INVALID_NODE_MARKERS"


def test_export_authored_lod_context_is_checked_without_renaming():
    assert validate_node_markers(
        "body", "MESH", authored_lod=2).lod_level == 2
    assert validate_node_markers(
        "body_lod02", "MESH", authored_lod=2).lod_level == 2
    with pytest.raises(MHValidationError) as exc:
        validate_node_markers("body_lod01", "MESH", authored_lod=2)
    assert exc.value.code == "MH_E_INVALID_LOD_HIERARCHY"


def test_plan_preserves_order_slots_and_parent_hierarchy():
    plan = build_mesh_import_plan("vehicle", [
        MeshNode("root", "NULL"),
        MeshNode("body_lod00", "MESH", "root", ("paint", "glass"),
                 "BodyMesh"),
        MeshNode("body_lod01", "MESH", "root", ("paint",), "BodyLow"),
        MeshNode("UCX_body", "MESH", "root", geometry_name="Collision"),
        MeshNode("SOCKET_grip", "NULL", "root"),
    ])
    assert plan.target_collection_name == "vehicle.lods"
    assert plan.lod_levels == (0, 1)
    assert plan.material_names == ("paint", "glass")
    assert [node.name for node in plan.nodes] == [
        "root", "body_lod00", "body_lod01", "UCX_body", "SOCKET_grip"]


def test_plan_rejects_mixed_and_sparse_lods():
    with pytest.raises(MHValidationError) as mixed:
        build_mesh_import_plan("mesh", [
            MeshNode("a_lod00", "MESH", geometry_name="A"),
            MeshNode("b", "MESH", geometry_name="B")])
    assert mixed.value.code == "MH_E_INVALID_LOD_HIERARCHY"
    with pytest.raises(MHValidationError) as sparse:
        build_mesh_import_plan("mesh", [
            MeshNode("a_lod00", "MESH", geometry_name="A"),
            MeshNode("b_lod02", "MESH", geometry_name="B")])
    assert sparse.value.code == "MH_E_LOD_LEVELS_SPARSE"


def test_plan_rejects_parent_escape_cycle_and_socket_children():
    with pytest.raises(MHValidationError) as escaped:
        build_mesh_import_plan("mesh", [
            MeshNode("body", "MESH", "outside", geometry_name="Body")])
    assert escaped.value.code == "MH_E_PARENT_OUTSIDE_RESOURCE"
    with pytest.raises(MHValidationError) as cycle:
        build_mesh_import_plan("mesh", [
            MeshNode("a", "NULL", "b"), MeshNode("b", "MESH", "a")])
    assert cycle.value.code == "MH_E_PARENT_CYCLE"
    with pytest.raises(MHValidationError) as socket:
        build_mesh_import_plan("mesh", [
            MeshNode("SOCKET_root", "NULL"),
            MeshNode("body", "MESH", "SOCKET_root", geometry_name="Body"),
        ])
    assert socket.value.code == "MH_E_INVALID_NODE_MARKERS"


def test_plan_rejects_slot_only_present_in_higher_lod():
    with pytest.raises(MHValidationError) as exc:
        build_mesh_import_plan("mesh", [
            MeshNode("body_lod00", "MESH", material_slots=("base",),
                     geometry_name="Body"),
            MeshNode("body_lod01", "MESH", material_slots=("high",),
                     geometry_name="BodyLow"),
        ])
    assert exc.value.code == "MH_E_LOD_SLOT_NOT_IN_BASE"


@pytest.mark.parametrize("node", [
    MeshNode("pivot", "NULL", material_slots=("paint",)),
    MeshNode("pivot", "NULL", geometry_name="PivotGeometry"),
])
def test_plan_rejects_payload_attached_to_null_model(node):
    with pytest.raises(MHValidationError) as exc:
        build_mesh_import_plan("mesh", [node])
    assert exc.value.code == "MH_E_UNSUPPORTED_NODE_KIND"


@pytest.mark.parametrize("nodes", [
    [MeshNode("body", "MESH", geometry_name="")],
    [
        MeshNode("body_a", "MESH", geometry_name="SharedGeometry"),
        MeshNode("body_b", "MESH", geometry_name="SharedGeometry"),
    ],
])
def test_plan_rejects_empty_or_duplicate_geometry_names(nodes):
    with pytest.raises(MHValidationError) as exc:
        build_mesh_import_plan("mesh", nodes)
    assert exc.value.code == "MH_E_IMPORT_TARGET_OCCUPIED"
