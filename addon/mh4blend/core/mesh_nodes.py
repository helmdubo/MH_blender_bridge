"""Pure node classification for the Source Protocol v4 mesh FBX dialect.

The FBX reader, Blender writer and (from S5) the UE importer all classify
nodes from the same small set of observable facts: node name, FBX node type,
parent relation and ordered material-slot names.  Keeping those rules free of
``bpy`` makes the fail-closed marker contract independently testable.
"""

from __future__ import annotations

from dataclasses import dataclass
import re
from typing import Iterable

from .canonical import validate_resource_name
from .validate import MHValidationError

__all__ = [
    "COLLISION_SHAPES",
    "COLLISION_TRANSPORT_KINDS",
    "DEFAULT_COLLISION_SHAPE",
    "FBX_COLLISION_KIND_KEY",
    "FBX_COLLISION_SHAPE_KEY",
    "FBX_PHMAT_KEY",
    "ClassifiedMeshNode",
    "MeshImportPlan",
    "MeshNode",
    "build_mesh_import_plan",
    "lod_ordered_material_union",
    "normalize_collision_transport",
    "strip_blender_duplicate_suffix",
    "validate_node_markers",
]


_CLASS_SUFFIX_RE = re.compile(r"_cls_(?P<mode>phys|trace|both)$")
_LOD_SUFFIX_RE = re.compile(r"_lod(?P<level>\d{2})$")
# Blender appends ``.001``..``.999`` when a name is already taken. The suffix
# belongs to the Blender datablock namespace, never to a transported logical
# resource name (docs/15 §2.3).
_BLENDER_DUPLICATE_SUFFIX_RE = re.compile(r"\.\d{3}$")

# V5-S6.1.2 collision transport (docs/15 §3.4).  ``phys`` becomes UE Simple
# Collision, ``trace`` becomes UE Complex Collision; the Dagor `both` spelling
# has no ratified UE meaning and is deliberately absent here.
COLLISION_TRANSPORT_KINDS = ("phys", "trace")
COLLISION_SHAPES = ("mesh", "box", "convex", "capsule")
DEFAULT_COLLISION_SHAPE = "mesh"
# The exact FBX Model user-property names of the frozen carrier.  Writer,
# reader and the UE importer all use these three strings and nothing else.
FBX_COLLISION_KIND_KEY = "mh_collision"
FBX_COLLISION_SHAPE_KEY = "mh_collision_shape"
FBX_PHMAT_KEY = "mh_phmat"
# The registry (docs/reference_notes/dagor_phmat_registry.md) may grow, so a
# token is validated by charset only; an unregistered token is legal transport
# and is resolved or warned about by the UE importer.
_PHMAT_TOKEN_RE = re.compile(r"^[A-Za-z0-9_]+$")


def _grammar_error(subjects, message) -> MHValidationError:
    return MHValidationError("MH_E_COMPOSITE_GRAMMAR", subjects, message)


def normalize_collision_transport(
        name: str,
        collision_kind,
        collision_shape=None,
        phmat=None,
) -> tuple[str, str, str | None]:
    """Validate one node's collision carrier and resolve its defaults.

    ``collision_shape`` defaults to ``mesh`` exactly as the Dagor script block
    does, and an absent ``phmat`` stays absent: the node is transported without
    a physical material rather than with an invented one.
    """

    if collision_kind is None:
        if collision_shape is not None or phmat is not None:
            raise _grammar_error(
                [name, FBX_COLLISION_KIND_KEY],
                f"collision node '{name}' declares "
                f"'{FBX_COLLISION_SHAPE_KEY}'/'{FBX_PHMAT_KEY}' without "
                f"'{FBX_COLLISION_KIND_KEY}'")
        return None, None, None
    if collision_kind not in COLLISION_TRANSPORT_KINDS:
        raise _grammar_error(
            [name, str(collision_kind)],
            f"'{FBX_COLLISION_KIND_KEY}' must be one of "
            f"{list(COLLISION_TRANSPORT_KINDS)}; got {collision_kind!r}")
    if collision_shape is None:
        collision_shape = DEFAULT_COLLISION_SHAPE
    elif collision_shape not in COLLISION_SHAPES:
        raise _grammar_error(
            [name, str(collision_shape)],
            f"'{FBX_COLLISION_SHAPE_KEY}' must be one of "
            f"{list(COLLISION_SHAPES)}; got {collision_shape!r}")
    if phmat is not None:
        if not isinstance(phmat, str) or _PHMAT_TOKEN_RE.fullmatch(phmat) is None:
            raise _grammar_error(
                [name, str(phmat)],
                f"'{FBX_PHMAT_KEY}' must be one [A-Za-z0-9_]+ token; got "
                f"{phmat!r}")
    return collision_kind, collision_shape, phmat


def strip_blender_duplicate_suffix(name: str) -> str:
    """Return ``name`` without one trailing Blender ``.NNN`` duplicate suffix."""
    if not isinstance(name, str):
        return name
    return _BLENDER_DUPLICATE_SUFFIX_RE.sub("", name)


@dataclass(frozen=True)
class MeshNode:
    """One parsed FBX Model node before semantic classification."""

    name: str
    node_type: str
    parent: str | None = None
    material_slots: tuple[str, ...] = ()
    geometry_name: str | None = None
    # V5-S6.1.2 FBX collision carrier, append-only (docs/15 §3.4).
    collision_kind: str | None = None
    collision_shape: str | None = None
    phmat: str | None = None


@dataclass(frozen=True)
class ClassifiedMeshNode:
    """Stable semantic result shared by export and import adapters."""

    name: str
    node_type: str
    kind: str
    lod_level: int | None = None
    collision_mode: str | None = None
    parent: str | None = None
    material_slots: tuple[str, ...] = ()
    geometry_name: str | None = None
    collision_shape: str | None = None
    phmat: str | None = None


@dataclass(frozen=True)
class MeshImportPlan:
    resource_name: str
    nodes: tuple[ClassifiedMeshNode, ...]
    lod_levels: tuple[int, ...]
    material_names: tuple[str, ...]
    uses_lod_collections: bool

    @property
    def target_collection_name(self) -> str:
        suffix = ".lods" if self.uses_lod_collections else ""
        return f"{self.resource_name}{suffix}"

    @property
    def collision_nodes(self) -> tuple[ClassifiedMeshNode, ...]:
        """Return the transported Dagor collision nodes in FBX Model order."""
        return tuple(
            node for node in self.nodes
            if node.kind == "collision"
            and node.collision_mode in COLLISION_TRANSPORT_KINDS)


def _marker_error(name: str, message: str) -> MHValidationError:
    return MHValidationError("MH_E_INVALID_NODE_MARKERS", [name], message)


def validate_node_markers(
        name: str,
        node_type: str,
        *,
        has_children: bool = False,
        authored_lod: int | None = None,
        collision_kind: str | None = None,
        collision_shape: str | None = None,
        phmat: str | None = None,
) -> ClassifiedMeshNode:
    """Classify one mesh-dialect node or reject conflicting name markers.

    ``node_type`` accepts the FBX terms ``MESH``/``NULL``; Blender's
    equivalent ``EMPTY`` is normalized to ``NULL`` for exporter callers.
    ``authored_lod`` is optional exporter context.  A terminal ``_lodNN``
    must match it exactly; missing suffix remains legal because the exporter
    applies its temporary name in a later context.

    ``collision_kind``/``collision_shape``/``phmat`` are the V5-S6.1.2 FBX
    carrier (docs/15 §3.4).  Real Dagor collision node names obey no marker
    convention at all (``gaz53_a_body.lod01 cls phys.001``), so the carrier —
    not the name — is what classifies a transported collision node.
    """

    if not isinstance(name, str) or not name:
        raise _marker_error(str(name), "FBX node name must be a non-empty string")
    normalized_type = str(node_type).upper()
    if normalized_type == "EMPTY":
        normalized_type = "NULL"
    if normalized_type not in {"MESH", "NULL"}:
        raise MHValidationError(
            "MH_E_UNSUPPORTED_NODE_KIND", [name],
            f"mesh FBX node '{name}' has unsupported type '{node_type}'")

    has_ucx = name.startswith("UCX_")
    has_socket = name.startswith("SOCKET_")
    class_match = _CLASS_SUFFIX_RE.search(name)
    lod_match = _LOD_SUFFIX_RE.search(name)

    collision_kind, collision_shape, phmat = normalize_collision_transport(
        name, collision_kind, collision_shape, phmat)
    if collision_kind is not None:
        if normalized_type != "MESH":
            raise _marker_error(
                name, "FBX null node cannot carry a collision transport claim")
        if has_ucx or has_socket or lod_match is not None or (
                class_match is not None
                and class_match.group("mode") != collision_kind):
            raise _marker_error(
                name,
                "collision transport claim conflicts with the node's name "
                "markers")
        return ClassifiedMeshNode(
            name=name, node_type=normalized_type, kind="collision",
            collision_mode=collision_kind, collision_shape=collision_shape,
            phmat=phmat)

    if normalized_type == "MESH":
        if has_socket:
            raise _marker_error(
                name, "SOCKET_ is valid only on an FBX null node")
        classification_markers = sum((has_ucx, class_match is not None,
                                      lod_match is not None))
        if classification_markers > 1:
            raise _marker_error(
                name, "mesh node carries more than one classification marker")
        if has_ucx or class_match is not None:
            mode = "both" if has_ucx else class_match.group("mode")
            return ClassifiedMeshNode(
                name=name, node_type=normalized_type, kind="collision",
                collision_mode=mode)

        level = int(lod_match.group("level")) if lod_match else 0
        if authored_lod is not None:
            if isinstance(authored_lod, bool) or not isinstance(authored_lod, int):
                raise TypeError("authored_lod must be an int or None")
            if authored_lod < 0 or authored_lod > 99:
                raise ValueError("authored_lod must be in range 0..99")
            if lod_match is not None and level != authored_lod:
                raise MHValidationError(
                    "MH_E_INVALID_LOD_HIERARCHY", [name],
                    f"mesh node '{name}' is authored in LOD {authored_lod} "
                    f"but carries {lod_match.group(0)}")
            level = authored_lod
        return ClassifiedMeshNode(
            name=name, node_type=normalized_type, kind="render",
            lod_level=level)

    # NULL nodes never carry mesh classification markers.
    if has_ucx or class_match is not None or lod_match is not None:
        raise _marker_error(
            name, "FBX null node carries a mesh classification marker")
    if has_socket:
        if has_children:
            raise _marker_error(name, "SOCKET_ null node cannot have children")
        return ClassifiedMeshNode(
            name=name, node_type=normalized_type, kind="socket")
    return ClassifiedMeshNode(
        name=name, node_type=normalized_type, kind="group")


def _validate_parent_graph(nodes: tuple[MeshNode, ...]) -> dict[str, list[str]]:
    by_name = {node.name: node for node in nodes}
    if len(by_name) != len(nodes):
        repeated = sorted({node.name for node in nodes
                           if sum(row.name == node.name for row in nodes) > 1})
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", repeated,
            "FBX contains duplicate Model node names")

    children = {name: [] for name in by_name}
    for node in nodes:
        if node.parent is None:
            continue
        if node.parent not in by_name:
            raise MHValidationError(
                "MH_E_PARENT_OUTSIDE_RESOURCE", [node.name, node.parent],
                f"parent '{node.parent}' is not a Model node in this resource")
        children[node.parent].append(node.name)

    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(name: str) -> None:
        if name in visited:
            return
        if name in visiting:
            raise MHValidationError(
                "MH_E_PARENT_CYCLE", [name], "FBX Model parent graph is cyclic")
        visiting.add(name)
        parent = by_name[name].parent
        if parent is not None:
            visit(parent)
        visiting.remove(name)
        visited.add(name)

    for name in by_name:
        visit(name)
    return children


def lod_ordered_material_union(
        nodes: Iterable[ClassifiedMeshNode]) -> tuple[str, ...]:
    """Return one static mesh's material list as the ordered union of its LODs.

    Owner decision 2026-08-30 (docs/15 §1.1) retired the earlier rule that
    every higher LOD had to reuse LOD0 slots: real Dagor content authors each
    LOD with its own materials.  The list is therefore the deterministic union

    * LOD0 slots in order of first appearance, then
    * slots newly introduced by LOD1 in order of first appearance, then LOD2 …

    The union is **render-only** (docs/15 §3.4, last bullet): a slot owned only
    by non-render nodes — ``UCX_`` collision or Dagor collision transport — is
    not a static-mesh material, so it neither joins this list nor enters the
    ``.material`` closure.  This is the S6.1.1 UCX tail retired, and it makes
    the Python list identical to the UE ``MaterialNames`` list.

    Node order inside one LOD is the FBX Model order, and slot order inside one
    node is the FBX material order, so writer, reader and the UE importer all
    derive the same list from the same bytes.  Each LOD then maps its own
    sections into this list.
    """
    ordered: list[str] = []
    seen: set[str] = set()

    render_nodes = [node for node in nodes if node.kind == "render"]
    for level in sorted({node.lod_level for node in render_nodes}):
        for node in render_nodes:
            if node.lod_level != level:
                continue
            for slot in node.material_slots:
                if slot not in seen:
                    seen.add(slot)
                    ordered.append(slot)
    return tuple(ordered)


def build_mesh_import_plan(
        resource_name: str,
        nodes: Iterable[MeshNode],
) -> MeshImportPlan:
    """Validate a parsed Model graph and freeze its import semantics."""

    try:
        validate_resource_name(resource_name)
    except (TypeError, ValueError) as exc:
        raise MHValidationError(
            "MH_E_NONCANONICAL_RESOURCE_NAME", [str(resource_name)], str(exc)) from exc

    parsed = tuple(nodes)
    if not parsed:
        raise MHValidationError(
            "MH_E_EMPTY_RESOURCE_COLLECTION", [resource_name],
            "mesh FBX contains no Model nodes")
    children = _validate_parent_graph(parsed)

    classified = []
    geometry_names = []
    any_lod_suffix = False
    any_plain_render = False
    for node in parsed:
        result = validate_node_markers(
            node.name, node.node_type,
            has_children=bool(children[node.name]),
            collision_kind=node.collision_kind,
            collision_shape=node.collision_shape,
            phmat=node.phmat)
        if result.node_type == "NULL" and (
                node.material_slots or node.geometry_name is not None):
            raise MHValidationError(
                "MH_E_UNSUPPORTED_NODE_KIND", [node.name],
                "FBX Null Model cannot own Geometry or material slots")
        if result.node_type == "MESH":
            if not isinstance(node.geometry_name, str) or not node.geometry_name:
                raise MHValidationError(
                    "MH_E_IMPORT_TARGET_OCCUPIED", [node.name],
                    "FBX Mesh Model requires one non-empty Geometry name")
            geometry_names.append(node.geometry_name)
        if result.kind == "render":
            if _LOD_SUFFIX_RE.search(node.name):
                any_lod_suffix = True
            else:
                any_plain_render = True
        for slot in node.material_slots:
            try:
                validate_resource_name(slot)
            except (TypeError, ValueError) as exc:
                raise MHValidationError(
                    "MH_E_NONCANONICAL_RESOURCE_NAME", [slot], str(exc)) from exc
        classified.append(ClassifiedMeshNode(
            name=result.name,
            node_type=result.node_type,
            kind=result.kind,
            lod_level=result.lod_level,
            collision_mode=result.collision_mode,
            parent=node.parent,
            material_slots=tuple(node.material_slots),
            geometry_name=node.geometry_name,
            collision_shape=result.collision_shape,
            phmat=result.phmat,
        ))

    duplicate_geometries = sorted({
        name for name in geometry_names if geometry_names.count(name) > 1
    })
    if duplicate_geometries:
        raise MHValidationError(
            "MH_E_IMPORT_TARGET_OCCUPIED", duplicate_geometries,
            "FBX Geometry names must be globally unique before Blender import")

    render_nodes = [node for node in classified if node.kind == "render"]
    if not render_nodes:
        raise MHValidationError(
            "MH_E_EMPTY_RESOURCE_COLLECTION", [resource_name],
            "mesh FBX contains no render mesh nodes")
    if any_lod_suffix and any_plain_render:
        raise MHValidationError(
            "MH_E_INVALID_LOD_HIERARCHY",
            [node.name for node in render_nodes],
            "LOD FBX mixes terminal _lodNN and unsuffixed render nodes")

    levels = sorted({node.lod_level for node in render_nodes})
    if any_lod_suffix:
        missing = sorted(set(range(max(levels) + 1)) - set(levels))
        if missing:
            raise MHValidationError(
                "MH_E_LOD_LEVELS_SPARSE",
                [f"lod{level:02d}" for level in missing],
                "FBX LOD levels must be contiguous from lod00")

    materials = lod_ordered_material_union(classified)
    return MeshImportPlan(
        resource_name=resource_name,
        nodes=tuple(classified),
        lod_levels=tuple(levels),
        material_names=materials,
        uses_lod_collections=any_lod_suffix,
    )
