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


def test_slot_only_present_in_higher_lod_joins_the_union():
    """Retired behavior: MH_E_LOD_SLOT_NOT_IN_BASE is never raised again.

    Owner decision 2026-08-30 (docs/15 §1.1): real content authors every LOD
    with its own materials, so the resource material list is the ordered union
    of all LOD slots instead of a subset of LOD0.
    """
    plan = build_mesh_import_plan("mesh", [
        MeshNode("body_lod00", "MESH", material_slots=("base",),
                 geometry_name="Body"),
        MeshNode("body_lod01", "MESH", material_slots=("high",),
                 geometry_name="BodyLow"),
    ])
    assert plan.material_names == ("base", "high")


def test_material_union_is_ordered_lod_major_by_first_appearance():
    plan = build_mesh_import_plan("mesh", [
        # Deliberately unsorted node order: the union order is defined by the
        # LOD level first and only then by node/slot appearance.
        MeshNode("hull_lod02", "MESH", material_slots=("simple",),
                 geometry_name="HullFar"),
        MeshNode("hull_lod00", "MESH", material_slots=("paint", "glass"),
                 geometry_name="Hull"),
        MeshNode("trim_lod01", "MESH", material_slots=("glass", "paint_low"),
                 geometry_name="TrimMid"),
        MeshNode("hull_lod01", "MESH", material_slots=("paint_low", "rubber"),
                 geometry_name="HullMid"),
    ])
    assert plan.lod_levels == (0, 1, 2)
    assert plan.material_names == (
        "paint", "glass", "paint_low", "rubber", "simple")


def test_material_union_is_render_only():
    """docs/15 §3.4 last bullet: both sides move to a render-only union.

    A slot owned only by non-render nodes (``UCX_`` collision, Dagor collision
    transport) is not a static-mesh material, so it never joins the union and
    never enters the ``.material`` closure.
    """
    plan = build_mesh_import_plan("mesh", [
        MeshNode("UCX_hull", "MESH", material_slots=("hull_shell",),
                 geometry_name="Collision"),
        MeshNode("hull_lod00", "MESH", material_slots=("paint",),
                 geometry_name="Hull"),
        MeshNode("hull_lod01", "MESH", material_slots=("paint_low",),
                 geometry_name="HullMid"),
    ])
    assert plan.material_names == ("paint", "paint_low")


def test_collision_transport_properties_classify_and_reach_the_plan():
    plan = build_mesh_import_plan("mesh", [
        MeshNode("hull_lod00", "MESH", material_slots=("paint",),
                 geometry_name="Hull"),
        MeshNode("gaz53_a_body.lod01 cls phys.001", "MESH",
                 geometry_name="PhysCollision", collision_kind="phys",
                 collision_shape="box", phmat="steel"),
        MeshNode("gaz53_a_body.lod01 cls steel.001", "MESH",
                 geometry_name="TraceCollision", collision_kind="trace",
                 phmat="wood"),
    ])
    phys, trace = plan.collision_nodes
    assert (phys.kind, phys.collision_mode, phys.collision_shape, phys.phmat) \
        == ("collision", "phys", "box", "steel")
    # An absent `mh_collision_shape` resolves to the Dagor default `mesh`.
    assert (trace.collision_mode, trace.collision_shape, trace.phmat) == (
        "trace", "mesh", "wood")
    # Transported collision is not bound to a LOD and never joins the union.
    assert plan.lod_levels == (0,)
    assert plan.material_names == ("paint",)


def test_collision_transport_node_without_phmat_is_legal():
    plan = build_mesh_import_plan("mesh", [
        MeshNode("hull", "MESH", geometry_name="Hull"),
        MeshNode("hull cls phys", "MESH", geometry_name="Collision",
                 collision_kind="phys"),
    ])
    assert plan.collision_nodes[0].phmat is None


@pytest.mark.parametrize("token", ["wood", "wood_solid", "softSteelDoor",
                                   "not_in_registry_2"])
def test_any_charset_valid_phmat_token_is_accepted(token):
    """docs/reference_notes/dagor_phmat_registry.md: the registry may grow.

    Only the token charset is a transport rule; an unknown token is legal and
    is resolved (or warned about) by the UE importer, never refused here.
    """
    plan = build_mesh_import_plan("mesh", [
        MeshNode("hull", "MESH", geometry_name="Hull"),
        MeshNode("hull cls phys", "MESH", geometry_name="Collision",
                 collision_kind="phys", phmat=token),
    ])
    assert plan.collision_nodes[0].phmat == token


@pytest.mark.parametrize(("kind", "shape", "phmat"), [
    ("simple", None, None),
    ("phys", "sphere", None),
    ("phys", "", None),
    ("phys", None, "steel wool"),
    ("phys", None, ""),
    ("phys", None, 7),
    (None, "box", None),
    (None, None, "steel"),
])
def test_malformed_collision_transport_is_a_grammar_refusal(kind, shape, phmat):
    with pytest.raises(MHValidationError) as exc:
        build_mesh_import_plan("mesh", [
            MeshNode("hull", "MESH", geometry_name="Hull"),
            MeshNode("hull cls phys", "MESH", geometry_name="Collision",
                     collision_kind=kind, collision_shape=shape, phmat=phmat),
        ])
    assert exc.value.code == "MH_E_COMPOSITE_GRAMMAR"


@pytest.mark.parametrize(("name", "node_type", "kind"), [
    ("UCX_hull", "MESH", "phys"),
    ("hull_lod00", "MESH", "phys"),
    ("hull_cls_trace", "MESH", "phys"),
    ("pivot", "NULL", "phys"),
])
def test_collision_property_conflicting_with_name_markers_fails_closed(
        name, node_type, kind):
    with pytest.raises(MHValidationError) as exc:
        validate_node_markers(name, node_type, collision_kind=kind)
    assert exc.value.code in {
        "MH_E_INVALID_NODE_MARKERS", "MH_E_UNSUPPORTED_NODE_KIND"}


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
