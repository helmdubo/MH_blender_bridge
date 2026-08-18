"""Dependency-aware standalone ``.composite`` import for Blender.

The importer first reads and validates the complete reachable composite graph.
Only after that preflight succeeds does it touch Blender data.  Definitions
and unresolved resources are represented by ordinary collections in the
``GEOMETRY`` scene; placements are Empty collection instances carrying the
source node metadata in Custom Properties.
"""

from __future__ import annotations

import contextlib
from copy import deepcopy
import json
import math
import os
import re
import uuid

import bpy
from mathutils import Matrix, Quaternion, Vector

from ..core.canonical import (
    ERROR_CODES,
    P_ROTATION_QUAT,
    canonicalize_quat,
    quantize,
    validate_resource_name,
)
from ..core.fbx_passport import read_fbx_passport
from ..core.materials import material_disk_payload
from ..core.material_source import (
    MaterialSourceError,
    prepare_material_export,
    validate_material_document,
)
from ..core.payload_publish_v2 import atomic_publish_json
from ..core.source_index_v2 import (
    SourceIndexV2Error,
    assert_resource_stable,
    assert_source_inventory_stable,
    canonical_payload_path,
    capture_resource_stability_token,
    capture_source_inventory,
    rebuild_and_publish_index,
    resolve_resource_index_first,
)
from ..core.texture_actualize import (
    actualize_material_document,
    assert_texture_tree_stable,
    capture_texture_tree,
)
from ..core.uid import PROP_UID
from ..core.validate import MHValidationError
from .composite_extract import (
    PROP_DISPLAY_NAME,
    PROP_KIND,
    PROP_PARENT_UID,
    PROP_PREFIX_PROPERTIES,
    PROP_RESOURCE_UID,
    _plain_id_property,
)
from .material_extract import (
    PROP_IMPORTED_MATERIAL_PAYLOAD,
    _dagormat_disk_payload,
)

__all__ = [
    "CompositeImportError",
    "load_composite_plan",
    "import_composite",
    "import_composite_file",
]

SUPPORTED_NODE_KINDS = frozenset({"group", "mesh", "composite_ref"})
RESERVED_NODE_KINDS = frozenset({"variant_set", "variant", "actor"})
_NODE_FIELDS = frozenset({
    "node_uid", "parent_uid", "kind", "display_name", "resource_uid",
    "local_transform", "properties",
})
_TRACKED_BLEND_DATA = (
    "objects", "collections", "scenes", "meshes", "materials",
    "armatures", "curves", "cameras", "lights", "images", "actions",
    "node_groups", "textures",
)


class CompositeImportError(ValueError):
    """A fail-closed input/preflight error with a stable diagnostic prefix."""

    def __init__(self, code, message):
        assert code in ERROR_CODES, f"unregistered import error code {code}"
        self.code = code
        super().__init__(f"{code}: {message}")


def _fail(code, message):
    raise CompositeImportError(code, message)


def _id_properties(carrier):
    return {str(key): _plain_id_property(carrier[key])
            for key in carrier.keys()}


def _restore_id_properties(carrier, values):
    for key in list(carrier.keys()):
        del carrier[key]
    for key, value in values.items():
        carrier[key] = deepcopy(value)


class _BlenderImportTransaction:
    """Rollback journal for every Blender ID mutation made by one import."""

    def __init__(self):
        self.baseline = {
            name: {item.as_pointer() for item in getattr(bpy.data, name)}
            for name in _TRACKED_BLEND_DATA
            if hasattr(bpy.data, name)
        }
        self.collection_states = {}
        self.object_states = {}
        self.material_states = {}
        self.added_scene_links = []

    def snapshot_collection(self, collection):
        key = collection.as_pointer()
        if key not in self.collection_states:
            self.collection_states[key] = (
                collection,
                collection.name,
                _id_properties(collection),
                tuple(collection.instance_offset),
            )

    def snapshot_object(self, obj):
        key = obj.as_pointer()
        if key not in self.object_states:
            self.object_states[key] = {
                "object": obj,
                "name": obj.name,
                "properties": _id_properties(obj),
                "parent": obj.parent,
                "matrix_parent_inverse": obj.matrix_parent_inverse.copy(),
                "matrix_basis": obj.matrix_basis.copy(),
                "instance_collection": obj.instance_collection,
                "instance_type": obj.instance_type,
                "empty_display_type": getattr(
                    obj, "empty_display_type", "PLAIN_AXES"),
                "color": tuple(obj.color),
                "collections": list(obj.users_collection),
            }

    def snapshot_material(self, material):
        key = material.as_pointer()
        if key in self.material_states:
            return
        dagormat = getattr(material, "dagormat", None)
        self.material_states[key] = {
            "material": material,
            "name": material.name,
            "properties": _id_properties(material),
            "dagormat": _id_properties(dagormat)
            if dagormat is not None else None,
            "textures": _id_properties(dagormat.textures)
            if dagormat is not None
            and getattr(dagormat, "textures", None) is not None else None,
            "optional": _id_properties(dagormat.optional)
            if dagormat is not None
            and getattr(dagormat, "optional", None) is not None else None,
        }

    def link_collection(self, scene, collection):
        if collection.name in scene.collection.children:
            return
        scene.collection.children.link(collection)
        self.added_scene_links.append((scene, collection))

    def _restore_existing(self):
        for state in self.material_states.values():
            material = state["material"]
            material.name = state["name"]
            _restore_id_properties(material, state["properties"])
            dagormat = getattr(material, "dagormat", None)
            if dagormat is not None and state["dagormat"] is not None:
                _restore_id_properties(dagormat, state["dagormat"])
                _restore_id_properties(
                    dagormat.textures, state["textures"] or {})
                _restore_id_properties(
                    dagormat.optional, state["optional"] or {})

        # Restore hierarchy links before transforms so parent-relative values
        # are evaluated against their original parents.
        for state in self.object_states.values():
            obj = state["object"]
            original_collections = state["collections"]
            for collection in list(obj.users_collection):
                if collection not in original_collections:
                    collection.objects.unlink(obj)
            for collection in original_collections:
                if collection not in obj.users_collection:
                    collection.objects.link(obj)
            obj.parent = state["parent"]
            obj.matrix_parent_inverse = state["matrix_parent_inverse"]
            obj.matrix_basis = state["matrix_basis"]
            obj.instance_collection = state["instance_collection"]
            obj.instance_type = state["instance_type"]
            if obj.type == "EMPTY":
                obj.empty_display_type = state["empty_display_type"]
            obj.color = state["color"]
            obj.name = state["name"]
            _restore_id_properties(obj, state["properties"])

        for collection, name, properties, instance_offset in \
                self.collection_states.values():
            collection.name = name
            _restore_id_properties(collection, properties)
            collection.instance_offset = instance_offset

        for scene, collection in reversed(self.added_scene_links):
            if collection.name in scene.collection.children:
                scene.collection.children.unlink(collection)

    def _release_restore_names(self):
        """Prevent Blender's automatic .001 suffixing during rollback."""
        targets = {
            "materials": {state["name"]
                          for state in self.material_states.values()},
            "objects": {state["name"]
                        for state in self.object_states.values()},
            "collections": {state[1]
                            for state in self.collection_states.values()},
        }
        for dataset_name, names in targets.items():
            baseline = self.baseline.get(dataset_name, set())
            for item in getattr(bpy.data, dataset_name):
                if item.as_pointer() not in baseline and item.name in names:
                    item.name = f"__mh_rollback__{item.as_pointer():x}"

    def _remove_new_ids(self):
        # Strong owners first. Remaining data blocks become unreferenced and
        # can then be removed without touching anything from the baseline.
        order = (
            "objects", "collections", "scenes", "meshes", "materials",
            "armatures", "curves", "cameras", "lights", "node_groups",
            "textures", "images", "actions",
        )
        for name in order:
            if name not in self.baseline:
                continue
            dataset = getattr(bpy.data, name)
            new_items = [item for item in dataset
                         if item.as_pointer() not in self.baseline[name]]
            for item in reversed(new_items):
                try:
                    dataset.remove(item, do_unlink=True)
                except TypeError:
                    with contextlib.suppress(RuntimeError, ReferenceError):
                        dataset.remove(item)
                except (RuntimeError, ReferenceError):
                    # Continue the rollback so one stubborn orphan cannot
                    # prevent restoration of the remaining baseline state.
                    pass

    def rollback(self):
        self._release_restore_names()
        self._restore_existing()
        self._remove_new_ids()

    def retire(self, collection, obj):
        """Commit-time unlink; snapshot makes an unexpected error reversible."""
        self.snapshot_object(obj)
        if collection in obj.users_collection:
            collection.objects.unlink(obj)

    @staticmethod
    def cleanup_retired(objects):
        # Product state is already committed after unlink. Datablock deletion
        # is best-effort and cannot turn a successful import into partial loss.
        retired_data = []
        for obj in objects:
            data = getattr(obj, "data", None)
            if data is not None:
                retired_data.append((obj.type, data))
            if obj.name in bpy.data.objects and not obj.users_collection:
                with contextlib.suppress(RuntimeError, ReferenceError):
                    bpy.data.objects.remove(obj, do_unlink=True)
        datasets = {
            "MESH": bpy.data.meshes,
            "CURVE": bpy.data.curves,
            "SURFACE": bpy.data.curves,
            "FONT": bpy.data.curves,
            "ARMATURE": bpy.data.armatures,
            "CAMERA": bpy.data.cameras,
            "LIGHT": bpy.data.lights,
        }
        for object_type, data in retired_data:
            dataset = datasets.get(object_type)
            if dataset is not None and data.users == 0:
                with contextlib.suppress(RuntimeError, ReferenceError):
                    dataset.remove(data)

    def cleanup_transport_ids(self):
        """Remove only zero-user IDs created during this import transaction."""
        order = (
            "objects", "collections", "materials", "meshes", "armatures",
            "curves", "cameras", "lights", "node_groups", "textures",
            "images", "actions",
        )
        # Removing an orphan material can release its image/node-group users;
        # a small fixed-point loop cleans that next layer without touching any
        # baseline or still-owned definition data.
        for _pass in range(3):
            removed = False
            for name in order:
                if name not in self.baseline:
                    continue
                dataset = getattr(bpy.data, name)
                candidates = [
                    item for item in dataset
                    if item.as_pointer() not in self.baseline[name]
                    and item.users == 0
                ]
                for item in reversed(candidates):
                    try:
                        dataset.remove(item)
                        removed = True
                    except (RuntimeError, ReferenceError, TypeError):
                        pass
            if not removed:
                break


def _read_json_bytes(path):
    try:
        with open(path, "rb") as stream:
            payload = stream.read()
        return json.loads(payload.decode("utf-8")), payload
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _fail("MH_E_INVALID_COMPOSITE", f"cannot read '{path}': {exc}")


def _read_json(path):
    return _read_json_bytes(path)[0]


def _number(value, subject):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail("MH_E_INVALID_COMPOSITE", f"{subject} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        _fail("MH_E_NAN_INF_VALUE", f"{subject} is NaN/Inf")
    return result


def _vector(value, count, subject):
    if not isinstance(value, list) or len(value) != count:
        _fail("MH_E_INVALID_COMPOSITE",
              f"{subject} must be an array of {count} numbers")
    return tuple(_number(component, subject) for component in value)


def _canonical_uid(value, subject):
    if not isinstance(value, str):
        _fail("MH_E_INVALID_COMPOSITE", f"{subject} must be a UUID string")
    try:
        canonical = str(uuid.UUID(value))
    except (ValueError, AttributeError, TypeError) as exc:
        _fail("MH_E_INVALID_COMPOSITE", f"{subject} is not a UUID: {exc}")
    if value != canonical:
        _fail("MH_E_INVALID_COMPOSITE",
              f"{subject} must use canonical lowercase UUID spelling")
    return canonical


def _validate_transform(value, node_uid):
    if not isinstance(value, dict):
        _fail("MH_E_INVALID_COMPOSITE",
              f"node {node_uid} local_transform must be an object")
    fields = {"translation_cm", "rotation_quat", "scale"}
    if set(value) != fields:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"node {node_uid} local_transform fields differ; "
            f"missing={sorted(fields - set(value))}, "
            f"unknown={sorted(set(value) - fields)}")
    translation = _vector(
        value.get("translation_cm"), 3,
        f"node {node_uid} translation_cm")
    rotation = _vector(
        value.get("rotation_quat"), 4,
        f"node {node_uid} rotation_quat")
    scale = _vector(value.get("scale"), 3, f"node {node_uid} scale")
    try:
        authored_quat = tuple(
            quantize(component, P_ROTATION_QUAT) for component in rotation)
        canonical_quat = canonicalize_quat(rotation)
    except (TypeError, ValueError, OverflowError) as exc:
        _fail("MH_E_INVALID_COMPOSITE",
              f"node {node_uid} has invalid quaternion: {exc}")
    if authored_quat != canonical_quat:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"node {node_uid} quaternion is not normalized and "
            "sign-canonicalized")
    if any(component <= 0.0 for component in scale):
        _fail("MH_E_INVALID_SCALE", f"node {node_uid} scale is {scale}")
    return {
        "translation_cm": translation,
        "rotation_quat": rotation,
        "scale": scale,
    }


def _json_bag(value, subject):
    if not isinstance(value, dict):
        _fail("MH_E_INVALID_COMPOSITE", f"{subject} must be an object")
    try:
        json.dumps(value, ensure_ascii=False, allow_nan=False)
    except (TypeError, ValueError) as exc:
        _fail("MH_E_INVALID_COMPOSITE",
              f"{subject} is not finite JSON: {exc}")
    return deepcopy(value)


def _validate_document(document, path):
    if not isinstance(document, dict):
        _fail("MH_E_INVALID_COMPOSITE", f"'{path}' is not a JSON object")
    if document.get("schema") != "mh.composite":
        _fail("MH_E_UNKNOWN_SCHEMA_VERSION",
              f"unsupported composite schema in '{path}'")
    version = document.get("schema_version")
    if (not isinstance(version, int) or isinstance(version, bool)
            or version != 2):
        _fail("MH_E_UNKNOWN_SCHEMA_VERSION",
              f"unsupported composite schema_version {version!r} in '{path}'")
    expected_fields = {
        "schema", "schema_version", "uid", "name", "properties", "nodes"}
    if set(document) != expected_fields:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"'{path}' has invalid fields; "
            f"missing={sorted(expected_fields - set(document))}, "
            f"unknown={sorted(set(document) - expected_fields)}")
    uid = _canonical_uid(document.get("uid"), f"'{path}' uid")
    name = document.get("name")
    if not isinstance(name, str) or not name:
        _fail("MH_E_INVALID_COMPOSITE", f"'{path}' has no name")
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        _fail("MH_E_NON_ASCII_RESOURCE_NAME", f"'{path}': {exc}")
    rows = document.get("nodes")
    if not isinstance(rows, list):
        _fail("MH_E_INVALID_COMPOSITE", f"'{path}' nodes must be an array")
    resource_properties = _json_bag(
        document.get("properties"), f"'{path}' properties")

    nodes = []
    by_uid = {}
    for raw in rows:
        if not isinstance(raw, dict):
            _fail("MH_E_INVALID_COMPOSITE", f"'{path}' node is not an object")
        node_uid = _canonical_uid(
            raw.get("node_uid"), f"'{path}' node_uid")
        if node_uid in by_uid:
            _fail("MH_E_DUPLICATE_NODE_UID", f"duplicate node {node_uid}")
        kind = raw.get("kind")
        if kind in RESERVED_NODE_KINDS:
            _fail("MH_E_UNSUPPORTED_NODE_KIND",
                  f"reserved node kind '{kind}' on {node_uid} is not "
                  "implemented; random/variant semantics were not guessed")
        if kind not in SUPPORTED_NODE_KINDS:
            _fail("MH_E_UNSUPPORTED_NODE_KIND",
                  f"unknown node kind '{kind}' on {node_uid}")
        expected_fields = _NODE_FIELDS if kind != "group" \
            else _NODE_FIELDS - {"resource_uid"}
        if set(raw) != expected_fields:
            _fail(
                "MH_E_INVALID_COMPOSITE",
                f"'{path}' node has invalid fields; "
                f"missing={sorted(expected_fields - set(raw))}, "
                f"unknown={sorted(set(raw) - expected_fields)}")
        display_name = raw.get("display_name")
        if not isinstance(display_name, str) or not display_name:
            _fail("MH_E_INVALID_COMPOSITE",
                  f"node {node_uid} has no display_name")
        parent_uid = raw.get("parent_uid")
        if parent_uid is not None:
            parent_uid = _canonical_uid(
                parent_uid, f"node {node_uid} parent_uid")
        resource_uid = raw.get("resource_uid")
        if kind == "group":
            if resource_uid is not None:
                _fail("MH_E_INVALID_COMPOSITE",
                      f"group node {node_uid} must not have resource_uid")
        else:
            resource_uid = _canonical_uid(
                resource_uid, f"node {node_uid} resource_uid")
        properties = _json_bag(
            raw.get("properties"), f"node {node_uid} properties")
        normalized = {
            "node_uid": node_uid,
            "parent_uid": parent_uid,
            "kind": kind,
            "display_name": display_name,
            "resource_uid": resource_uid,
            "local_transform": _validate_transform(
                raw.get("local_transform"), node_uid),
            "properties": deepcopy(properties),
            "custom_metadata": {},
        }
        nodes.append(normalized)
        by_uid[node_uid] = normalized

    for node in nodes:
        parent_uid = node["parent_uid"]
        if parent_uid is not None and parent_uid not in by_uid:
            _fail("MH_E_DANGLING_PARENT",
                  f"node {node['node_uid']} references missing parent "
                  f"{parent_uid}")
    for node in nodes:
        visited = set()
        cursor = node
        while cursor["parent_uid"] is not None:
            if cursor["node_uid"] in visited:
                _fail("MH_E_PARENT_CYCLE",
                      f"parent cycle containing {sorted(visited)}")
            visited.add(cursor["node_uid"])
            cursor = by_uid[cursor["parent_uid"]]
    return {
        "schema_version": version,
        "uid": uid,
        "name": name,
        "properties": deepcopy(resource_properties),
        "nodes": nodes,
        "path": path,
    }


def _fbx_passport_copies(path):
    """Adapt the production Carrier B reader to the pure index callback."""
    receipt = read_fbx_passport(path)
    return tuple(receipt.canonical_text for _ in range(receipt.copy_count))


def _resolve_v2(source_root, uid, expected_kind, *, index_path, lock_root):
    try:
        resolved = resolve_resource_index_first(
            source_root, uid,
            fbx_passport_extractor=_fbx_passport_copies,
            index_path=index_path, lock_root=lock_root)
    except SourceIndexV2Error as exc:
        if exc.code == "MH_E_RESOURCE_NOT_FOUND":
            return None
        raise
    if resolved.kind != expected_kind:
        _fail(
            "MH_E_RESOURCE_KIND_MISMATCH",
            f"resource {uid} is {resolved.kind!r}, expected {expected_kind!r}")
    return resolved


def load_composite_plan(
        filepath, *, source_root=None, texture_policy="transitional",
        index_path=None, lock_root=None):
    """Preflight one v2 root and its recursively reachable clean payloads."""
    root_path = os.path.abspath(bpy.path.abspath(filepath))
    root_directory = os.path.dirname(root_path)
    source_root = os.path.abspath(
        bpy.path.abspath(os.fspath(source_root or root_directory)))
    source_inventory_token = capture_source_inventory(source_root)
    root_payload, root_bytes = _read_json_bytes(root_path)
    root = _validate_document(root_payload, root_path)
    documents = {root["uid"]: root}
    document_paths = {root["uid"]: root_path}
    resources = {
        root["uid"]: {
            "uid": root["uid"], "kind": "composite", "name": root["name"],
            "source": root_path, "properties": deepcopy(root["properties"]),
        },
    }
    unresolved = set()
    materials = {}
    material_warnings = []
    resolver_warnings = []
    stability_tokens = {}
    resolved_cache = {}

    def resolve(uid, expected_kind):
        key = (uid, expected_kind)
        if key not in resolved_cache:
            resolved_cache[key] = _resolve_v2(
                source_root, uid, expected_kind,
                index_path=index_path, lock_root=lock_root)
            resolved = resolved_cache[key]
            if resolved is not None:
                for diagnostic in resolved.diagnostics:
                    code = diagnostic.split(":", 1)[0]
                    if code == "MH_W_DUPLICATE_IDENTICAL_PAYLOAD":
                        resolver_warnings.append(_warning(
                            code, [uid], diagnostic))
        return resolved_cache[key]

    resolved_root = resolve(root["uid"], "composite")
    if resolved_root is None:
        _fail(
            "MH_E_RESOURCE_NOT_FOUND",
            f"selected root composite is outside the v2 source snapshot: "
            f"{root_path}")
    root_candidates = resolved_root.index["uids"][root["uid"]][
        "candidate_paths"]
    if canonical_payload_path(root_path) not in root_candidates:
        _fail(
            "MH_E_RESOURCE_UID_MISMATCH",
            f"selected root path is not a candidate for {root['uid']}: "
            f"{root_path}")
    if resolved_root.parsed_passport["name"] != root["name"]:
        _fail(
            "MH_E_NAME_MISMATCH",
            f"selected root name {root['name']!r} disagrees with index")
    stability_tokens[root["uid"]] = \
        capture_resource_stability_token(resolved_root)

    queue = [root]
    while queue:
        document = queue.pop(0)
        for node in document["nodes"]:
            uid = node["resource_uid"]
            if uid is None:
                continue
            expected = "composite" if node["kind"] == "composite_ref" \
                else "static_mesh"
            prior = resources.get(uid)
            if prior is not None and prior["kind"] != expected:
                _fail("MH_E_RESOURCE_KIND_MISMATCH",
                      f"resource {uid} is referenced as both kinds")

            # The explicitly selected root path is authoritative even for a
            # recursive back-edge to itself. Other definitions were already
            # resolved and validated on first encounter.
            if uid in documents:
                loaded = documents[uid]
                resources[uid] = {
                    "uid": uid, "kind": "composite", "name": loaded["name"],
                    "source": document_paths[uid],
                    "properties": deepcopy(loaded["properties"]),
                }
                continue

            resolved = resolve(uid, expected)
            if resolved is None:
                unresolved.add(uid)
                resources[uid] = {
                    "uid": uid, "kind": expected,
                    "name": node["display_name"], "source": None,
                    "properties": {}, "material_slots": [],
                }
                continue
            token = capture_resource_stability_token(resolved)
            stability_tokens[uid] = token
            descriptor = resolved.parsed_passport
            source = resolved.payload_path
            if expected == "static_mesh":
                resources[uid] = {
                    "uid": uid, "kind": expected,
                    "name": descriptor["name"], "source": source,
                    "properties": deepcopy(descriptor["properties"]),
                    "material_slots": deepcopy(descriptor["material_slots"]),
                }
                continue

            loaded_payload, _loaded_bytes = _read_json_bytes(source)
            loaded = _validate_document(loaded_payload, source)
            if loaded["uid"] != uid:
                _fail("MH_E_RESOURCE_UID_MISMATCH",
                      f"'{source}' uid {loaded['uid']} != requested uid {uid}")
            if loaded["name"] != descriptor["name"]:
                _fail("MH_E_NAME_MISMATCH",
                      f"'{source}' name {loaded['name']!r} != indexed name "
                      f"{descriptor['name']!r}")
            documents[uid] = loaded
            document_paths[uid] = source
            resources[uid] = {
                "uid": uid, "kind": "composite", "name": loaded["name"],
                "source": source,
                "properties": deepcopy(loaded["properties"]),
            }
            queue.append(loaded)

    referenced_material_uids = sorted({
        slot["material_uid"]
        for resource in resources.values()
        if resource["kind"] == "static_mesh"
        for slot in resource.get("material_slots", [])
    })
    for material_uid in referenced_material_uids:
        resolved = resolve(material_uid, "material")
        if resolved is None:
            continue
        stability_tokens[material_uid] = \
            capture_resource_stability_token(resolved)
        material_path = resolved.payload_path
        try:
            document = _read_json(material_path)
            normalized, diagnostics = validate_material_document(
                document, source_root=source_root,
                texture_policy=texture_policy)
        except MaterialSourceError as exc:
            _fail(exc.code, f"invalid material {material_uid}: {exc}")
        if normalized["uid"] != material_uid:
            _fail(
                "MH_E_RESOURCE_UID_MISMATCH",
                f"material payload '{material_path}' uid "
                f"{normalized['uid']} does not match requested {material_uid}")
        if normalized["name"] != resolved.parsed_passport["name"]:
            _fail(
                "MH_E_NAME_MISMATCH",
                f"material payload '{material_path}' name "
                f"{normalized['name']!r} does not match indexed name "
                f"{resolved.parsed_passport['name']!r}")
        materials[material_uid] = normalized
        material_warnings.extend(diagnostic.disk_dict()
                                 for diagnostic in diagnostics)

    graph = {uid: [] for uid in documents}
    for uid, document in documents.items():
        graph[uid] = [
            (node["resource_uid"], node) for node in document["nodes"]
            if node["kind"] == "composite_ref"
            and node["resource_uid"] in documents
        ]
    colors = {uid: 0 for uid in graph}
    cycle_warnings = []

    def visit(uid, stack):
        colors[uid] = 1
        stack.append(uid)
        for target_uid, node in graph[uid]:
            if colors[target_uid] == 1:
                cycle = stack[stack.index(target_uid):] + [target_uid]
                node["cycle_placeholder"] = True
                cycle_warnings.append(_warning(
                    "MH_W_COMPOSITE_CYCLE", cycle,
                    "composite back-edge imported as an unresolved "
                    f"placeholder: {' -> '.join(cycle)}"))
                continue
            if colors[target_uid] == 0:
                visit(target_uid, stack)
        stack.pop()
        colors[uid] = 2

    for uid in sorted(graph):
        if colors[uid] == 0:
            visit(uid, [])

    for document in documents.values():
        for node in document["nodes"]:
            node["unresolved_placeholder"] = (
                node["resource_uid"] is not None
                and node["resource_uid"] in unresolved)

    assert_source_inventory_stable(source_inventory_token)
    return {
        "root_uid": root["uid"],
        "documents": documents,
        "resources": resources,
        "unresolved": sorted(unresolved),
        "materials": materials,
        "warnings": resolver_warnings + material_warnings + cycle_warnings,
        "root_payload_token": (root_path, root_bytes),
        "stability_tokens": tuple(stability_tokens.values()),
        "source_inventory_token": source_inventory_token,
        "source_root": source_root,
        "index_path": index_path,
        "lock_root": lock_root,
    }


def _assert_plan_stable(plan):
    assert_source_inventory_stable(plan["source_inventory_token"])
    root_path, expected = plan["root_payload_token"]
    try:
        with open(root_path, "rb") as stream:
            actual = stream.read()
    except OSError as exc:
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"root composite disappeared during import: {root_path}: {exc}") \
            from exc
    if actual != expected:
        raise SourceIndexV2Error(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"root composite changed during import: {root_path}")
    for token in plan["stability_tokens"]:
        assert_resource_stable(
            token,
            fbx_passport_extractor=(
                _fbx_passport_copies if token.kind == "static_mesh" else None))
        try:
            current = _resolve_v2(
                plan["source_root"], token.resource_uid, token.kind,
                index_path=plan["index_path"], lock_root=plan["lock_root"])
        except SourceIndexV2Error as exc:
            raise SourceIndexV2Error(
                "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                f"resource candidate set changed during import: "
                f"{token.resource_uid}: {exc}") from exc
        if (current is None
                or current.payload_path != token.payload_path
                or current.payload_fingerprint != token.payload_fingerprint
                or current.descriptor_hash != token.descriptor_hash):
            raise SourceIndexV2Error(
                "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                f"resolved resource changed during import: "
                f"{token.resource_uid}")
    for uid in plan["unresolved"]:
        expected_kind = plan["resources"][uid]["kind"]
        appeared = _resolve_v2(
            plan["source_root"], uid, expected_kind,
            index_path=plan["index_path"], lock_root=plan["lock_root"])
        if appeared is not None:
            raise SourceIndexV2Error(
                "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                f"previously missing resource appeared during import: {uid}")


def _find_collection_by_uid(uid):
    matches = [collection for collection in bpy.data.collections
               if collection.get(PROP_UID) == uid]
    if len(matches) > 1:
        _fail("MH_E_DUPLICATE_RESOURCE_UID",
              f"{len(matches)} Blender collections have uid {uid}")
    return matches[0] if matches else None


def _preflight_material_ownership(plan):
    referenced = {
        slot["material_uid"]
        for resource in plan["resources"].values()
        if resource["kind"] == "static_mesh"
        for slot in resource.get("material_slots", [])
        if slot["material_uid"] in plan["materials"]
    }
    incoming_names = {}
    existing = {}
    for uid in sorted(referenced):
        row = plan["materials"][uid]
        name = row["name"]
        previous_uid = incoming_names.get(name)
        if previous_uid is not None and previous_uid != uid:
            _fail("MH_E_TARGET_NAME_COLLISION",
                  f"materials {previous_uid} and {uid} both request "
                  f"Blender name '{name}'")
        incoming_names[name] = uid

        uid_owners = [material for material in bpy.data.materials
                      if material.get(PROP_UID) == uid]
        if len(uid_owners) > 1:
            _fail("MH_E_DUPLICATE_RESOURCE_UID",
                  f"{len(uid_owners)} Blender materials have uid {uid}")
        owner = uid_owners[0] if uid_owners else None
        if owner is not None:
            if owner.library is not None:
                _fail("MH_E_FOREIGN_UID_OWNER",
                      f"material '{owner.name}' ({uid}) is linked/read-only")

        name_owner = bpy.data.materials.get(name)
        if name_owner is not None and name_owner is not owner:
            other_uid = name_owner.get(PROP_UID)
            ownership = (f"uid {other_uid}" if other_uid
                         else "no mh_uid")
            _fail("MH_E_TARGET_NAME_COLLISION",
                  f"material name '{name}' is already owned by "
                  f"'{name_owner.name}' ({ownership})")
        existing[uid] = owner
    return existing


def _preflight_destination(plan):
    kinds = {}
    names = {}
    for uid, resource in plan["resources"].items():
        kinds[uid] = resource["kind"]
        names[uid] = resource["name"]
    for uid, document in plan["documents"].items():
        if uid in kinds and kinds[uid] != "composite":
            _fail("MH_E_RESOURCE_KIND_MISMATCH",
                  f"document {uid} collides with a mesh resource")
        kinds[uid] = "composite"
        names[uid] = document["name"]

    existing = {}
    incoming_names = {}
    for uid in kinds:
        expected_name = names[uid]
        prior_uid = incoming_names.get(expected_name)
        if prior_uid is not None and prior_uid != uid:
            _fail("MH_E_TARGET_NAME_COLLISION",
                  f"resources {prior_uid} and {uid} both request "
                  f"Blender collection name '{expected_name}'")
        incoming_names[expected_name] = uid
        collection = _find_collection_by_uid(uid)
        if collection is not None:
            declared = collection.get(PROP_KIND)
            if declared in {"composite", "static_mesh"} \
                    and declared != kinds[uid]:
                _fail("MH_E_RESOURCE_KIND_MISMATCH",
                      f"collection '{collection.name}' is {declared}, "
                      f"incoming {uid} is {kinds[uid]}")
            if collection.library is not None:
                _fail("MH_E_FOREIGN_UID_OWNER",
                      f"collection '{collection.name}' is linked/read-only")
            existing[uid] = collection
        name_owner = bpy.data.collections.get(expected_name)
        if name_owner is not None and name_owner is not collection:
            other_uid = name_owner.get(PROP_UID)
            ownership = f"uid {other_uid}" if other_uid else "no mh_uid"
            _fail("MH_E_TARGET_NAME_COLLISION",
                  f"collection name '{expected_name}' is already owned by "
                  f"'{name_owner.name}' ({ownership})")

    for uid, document in plan["documents"].items():
        collection = existing.get(uid)
        if collection is None:
            continue
        by_node_uid = {}
        for obj in collection.objects:
            node_uid = obj.get(PROP_UID)
            if not node_uid:
                continue
            by_node_uid.setdefault(node_uid, []).append(obj)
        for node_uid, objects in by_node_uid.items():
            if len(objects) > 1:
                _fail("MH_E_DUPLICATE_NODE_UID",
                      f"collection '{collection.name}' has duplicate "
                      f"node {node_uid}")
            if node_uid in {node["node_uid"] for node in document["nodes"]} \
                    and objects[0].type != "EMPTY":
                _fail("MH_E_TARGET_NAME_COLLISION",
                      f"incoming node {node_uid} is not an Empty")
            if objects[0].library is not None:
                _fail("MH_E_FOREIGN_UID_OWNER",
                      f"node {node_uid} is linked/read-only")
    material_existing = _preflight_material_ownership(plan)
    return kinds, names, existing, material_existing


def _link_to_scene(scene, collection, transaction=None):
    if transaction is not None:
        transaction.link_collection(scene, collection)
    elif collection.name not in scene.collection.children:
        scene.collection.children.link(collection)


def _set_properties(carrier, properties):
    for key in list(carrier.keys()):
        if isinstance(key, str) and key.startswith(PROP_PREFIX_PROPERTIES):
            del carrier[key]
    fallback = {}
    for key, value in properties.items():
        property_key = f"{PROP_PREFIX_PROPERTIES}{key}"
        if value is None:
            # IDProperties use None as a deletion sentinel rather than a
            # storable JSON null.
            fallback[key] = None
            continue
        try:
            carrier[property_key] = deepcopy(value)
            # Blender treats assignment of None as property deletion without
            # raising, so confirm that the value actually became representable.
            if property_key not in carrier:
                fallback[key] = deepcopy(value)
        except (TypeError, ValueError):
            # Arbitrary JSON remains lossless and inspectable even if Blender's
            # IDProperty implementation rejects a particular nested shape.
            fallback[key] = deepcopy(value)
    if fallback:
        carrier["mh_properties_fallback_json"] = json.dumps(
            fallback, ensure_ascii=False, sort_keys=True)
    elif "mh_properties_fallback_json" in carrier:
        del carrier["mh_properties_fallback_json"]


def _local_matrix(transform):
    tx, ty, tz = transform["translation_cm"]
    qx, qy, qz, qw = transform["rotation_quat"]
    sx, sy, sz = transform["scale"]
    location = Vector((tx / 100.0, -ty / 100.0, tz / 100.0))
    rotation = Quaternion((qw, -qx, qy, -qz))
    rotation.normalize()
    return Matrix.LocRotScale(location, rotation, Vector((sx, sy, sz)))


def _import_fbx_into_collection(scene, collection, source, resource_uid):
    before = {obj.as_pointer() for obj in bpy.data.objects}
    window = bpy.context.window
    prior_scene = window.scene if window is not None else None
    try:
        if window is not None:
            window.scene = scene
        if hasattr(bpy.ops.wm, "fbx_import"):
            result = bpy.ops.wm.fbx_import(filepath=source)
        elif hasattr(bpy.ops.import_scene, "fbx"):
            result = bpy.ops.import_scene.fbx(filepath=source)
        else:
            raise RuntimeError("no Blender FBX import operator is available")
        if "FINISHED" not in result:
            raise RuntimeError(f"FBX operator returned {set(result)}")
    finally:
        if window is not None and prior_scene is not None:
            window.scene = prior_scene

    imported = [obj for obj in bpy.data.objects
                if obj.as_pointer() not in before]
    if not imported:
        raise RuntimeError(f"FBX import created no objects: {source}")
    previous = [obj for obj in collection.objects
                if obj.get("mh_imported_resource_uid") == resource_uid]
    for obj in imported:
        if collection not in obj.users_collection:
            collection.objects.link(obj)
        for owner in list(obj.users_collection):
            if owner != collection:
                owner.objects.unlink(obj)
        obj["mh_imported_resource_uid"] = resource_uid
    # Previous payload objects remain linked until the entire dependency graph
    # and material pass succeeds. The transaction retires them at commit.
    return imported, previous


def _warning(code, subjects, message):
    assert code in ERROR_CODES and code.startswith("MH_W_")
    return {
        "code": code,
        "subjects": sorted(set(subjects)),
        "message": message,
    }


def _material_payload_json(row):
    return json.dumps(
        row, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _hydrate_dagormat_if_available(material, row):
    """Atomically hydrate dagormat and verify its complete canonical payload.

    Direct IDProperty writes on the registered PropertyGroups deliberately
    bypass the RNA update callbacks which invoke ``build_dagormat_node_tree``.
    Dagormat only becomes authoritative when the exporter-facing RNA readback
    is exactly the canonical standalone payload. On any unsupported value or
    partial write, the prior RNA state is restored and its shader is cleared,
    leaving the lossless imported JSON fallback authoritative.

    Returns ``(authoritative, reason)`` for a structured importer warning.
    """
    dagormat = getattr(material, "dagormat", None)
    if dagormat is None:
        return False, "dagormat RNA is unavailable"

    params = deepcopy(row["params"])
    optional = {key: value for key, value in params.items()
                if key != "sides"}
    sides = params.get("sides", 0)
    texture_group = getattr(dagormat, "textures", None)
    optional_group = getattr(dagormat, "optional", None)
    prior = _id_properties(dagormat)
    prior_textures = _id_properties(texture_group) \
        if texture_group is not None else None
    prior_optional = _id_properties(optional_group) \
        if optional_group is not None else None

    def restore_and_disable():
        _restore_id_properties(dagormat, prior)
        if texture_group is not None:
            _restore_id_properties(texture_group, prior_textures or {})
        if optional_group is not None:
            _restore_id_properties(optional_group, prior_optional or {})
        # A non-empty shader makes material_extract prefer dagormat over the
        # imported JSON. Clearing it is the explicit authority switch.
        dagormat["shader_class"] = ""

    try:
        dagormat["shader_class"] = row["shader_class"]
        dagormat["sides"] = int(sides)
        if texture_group is not None:
            for key in list(texture_group.keys()):
                del texture_group[key]
            for slot, path in row["textures"].items():
                texture_group[str(slot)] = str(path)
        if optional_group is not None:
            for key in list(optional_group.keys()):
                del optional_group[key]
            for key, value in optional.items():
                optional_group[str(key)] = deepcopy(value)
        expected = material_disk_payload(
            row["shader_class"], row["params"], row["textures"])
        actual = _dagormat_disk_payload(material)
    except (AttributeError, KeyError, TypeError, ValueError, RuntimeError,
            MHValidationError) as exc:
        restore_and_disable()
        return False, f"dagormat rejected the canonical payload: {exc}"
    if actual != expected:
        restore_and_disable()
        return False, "dagormat RNA readback differs from the canonical payload"
    return True, None


def _rehydrate_imported_materials(resource_uid, resource, objects,
                                   materials, existing_materials,
                                   transaction, warnings):
    slot_rows = resource.get("material_slots", [])
    expected = {row["slot_name"]: row["material_uid"] for row in slot_rows}
    actual = {}
    for obj in objects:
        if obj.type != "MESH":
            continue
        for slot in obj.material_slots:
            slot_name = str(slot.name or (
                slot.material.name if slot.material is not None else ""))
            if slot_name:
                actual.setdefault(slot_name, []).append(slot)

    hydrated = set()
    matched_actual_names = set()
    for slot_name, material_uid in expected.items():
        slots = actual.get(slot_name, [])
        actual_name = slot_name if slots else None
        if not slots:
            # Blender suffixes an imported material when a legitimate existing
            # UID owner already has the canonical name. Accept exactly one
            # unowned .### transport suffix, never an ambiguous set.
            suffixed = [name for name in actual
                        if re.sub(r"\.\d{3}$", "", name) == slot_name]
            if len(suffixed) == 1:
                actual_name = suffixed[0]
                slots = actual[actual_name]
        row = materials.get(material_uid)
        if not slots:
            warnings.append(_warning(
                "MH_W_MATERIAL_SLOT_NOT_FOUND", [resource_uid],
                f"mesh {resource_uid} FBX has no slot named '{slot_name}'"))
            continue
        matched_actual_names.add(actual_name)
        if row is None:
            continue

        material = existing_materials.get(material_uid)
        if material is None:
            material = next(
                (slot.material for slot in slots if slot.material is not None),
                None)
        if material is None:
            material = bpy.data.materials.new(row["name"])
        transaction.snapshot_material(material)
        for slot in slots:
            transport = slot.material
            if transport is not None and transport is not material \
                    and transport.name == row["name"]:
                transport.name = f"__mh_transport__{transport.as_pointer():x}"
        material.name = row["name"]
        material[PROP_UID] = material_uid
        material[PROP_IMPORTED_MATERIAL_PAYLOAD] = _material_payload_json(row)
        dagormat_authoritative, fallback_reason = \
            _hydrate_dagormat_if_available(material, row)
        if not dagormat_authoritative:
            warnings.append(_warning(
                "MH_W_MATERIAL_PAYLOAD_FALLBACK", [material_uid],
                f"material {material_uid} keeps "
                f"{PROP_IMPORTED_MATERIAL_PAYLOAD} authoritative: "
                f"{fallback_reason}"))
        for slot in slots:
            slot.material = material
        hydrated.add(material_uid)

    for slot_name in sorted(set(actual) - matched_actual_names):
        warnings.append(_warning(
            "MH_W_MATERIAL_SLOT_UNMAPPED", [resource_uid],
            f"mesh {resource_uid} FBX slot '{slot_name}' has no passport "
            "material mapping"))
    return hydrated


def _apply_document(collection, document, collections, transaction):
    incoming = {node["node_uid"]: node for node in document["nodes"]}
    existing = {obj.get(PROP_UID): obj for obj in collection.objects
                if obj.type == "EMPTY" and obj.get(PROP_UID)}
    objects = {}
    for node_uid, node in incoming.items():
        obj = existing.get(node_uid)
        if obj is None:
            obj = bpy.data.objects.new(node["display_name"], None)
            collection.objects.link(obj)
        else:
            transaction.snapshot_object(obj)
        obj.name = node["display_name"]
        is_placeholder = bool(
            node.get("cycle_placeholder")
            or node.get("unresolved_placeholder"))
        obj.empty_display_type = "CUBE" if is_placeholder else "PLAIN_AXES"
        obj.color = (1.0, 0.0, 0.0, 1.0) if is_placeholder \
            else (1.0, 1.0, 1.0, 1.0)
        obj[PROP_UID] = node_uid
        obj[PROP_KIND] = node["kind"]
        obj[PROP_DISPLAY_NAME] = node["display_name"]
        obj[PROP_PARENT_UID] = node["parent_uid"] or ""
        # Inspection snapshots make every source attribute visible in the
        # Custom Properties panel. They are informational: matrix_basis is the
        # authoritative editable transform used by the next export.
        obj["mh_translation_cm"] = list(
            node["local_transform"]["translation_cm"])
        obj["mh_rotation_quat"] = list(
            node["local_transform"]["rotation_quat"])
        obj["mh_scale"] = list(node["local_transform"]["scale"])
        obj["mh_instance_offset"] = [0.0, 0.0, 0.0]
        if node["resource_uid"] is None:
            if PROP_RESOURCE_UID in obj:
                del obj[PROP_RESOURCE_UID]
            obj.instance_collection = None
            obj.instance_type = "NONE"
        else:
            obj[PROP_RESOURCE_UID] = node["resource_uid"]
            if node.get("cycle_placeholder"):
                obj.instance_collection = None
                obj.instance_type = "NONE"
            else:
                obj.instance_type = "COLLECTION"
                obj.instance_collection = collections[node["resource_uid"]]
        obj["mh_unresolved"] = is_placeholder
        obj["mh_imported_composite_node"] = True
        _set_properties(obj, node["properties"])
        metadata = node["custom_metadata"]
        if metadata:
            obj["mh_custom_metadata_json"] = json.dumps(
                metadata, ensure_ascii=False, sort_keys=True)
        elif "mh_custom_metadata_json" in obj:
            del obj["mh_custom_metadata_json"]
        objects[node_uid] = obj

    # Parenting and local matrices are a separate pass so forward parent
    # references are harmless and matrix_basis remains authoritative.
    for node_uid, node in incoming.items():
        obj = objects[node_uid]
        obj.parent = objects.get(node["parent_uid"])
        obj.matrix_parent_inverse = Matrix.Identity(4)
        obj.matrix_basis = _local_matrix(node["local_transform"])

    return [obj for node_uid, obj in existing.items()
            if node_uid not in incoming
            and obj.get("mh_imported_composite_node")]


def import_composite(filepath, geometry_scene=None, import_fbx=True,
                     source_root=None, texture_policy="transitional",
                     index_path=None, lock_root=None):
    """Import a dependency graph into collection definitions in GEOMETRY.

    Existing collections with the same composite UID are updated in place.
    Invalid documents and destination UID conflicts are rejected during
    preflight, before any scene/datablock mutation. Composite back-edges are
    represented by unresolved warning placeholders so the remaining graph can
    still be edited in Blender.
    """
    plan = load_composite_plan(
        filepath, source_root=source_root, texture_policy=texture_policy,
        index_path=index_path, lock_root=lock_root)
    kinds, names, existing, existing_materials = _preflight_destination(plan)
    transaction = _BlenderImportTransaction()
    _assert_plan_stable(plan)
    retired = []
    try:
        scene = geometry_scene or bpy.data.scenes.get("GEOMETRY")
        if scene is None:
            scene = bpy.data.scenes.new("GEOMETRY")

        collections = {}
        for uid in sorted(kinds):
            collection = existing.get(uid)
            if collection is None:
                collection = bpy.data.collections.new(names[uid])
            transaction.snapshot_collection(collection)
            collection.name = names[uid]
            collection[PROP_UID] = uid
            collection[PROP_KIND] = kinds[uid]
            collection.instance_offset = (0.0, 0.0, 0.0)
            resource = plan["resources"].get(uid)
            if resource and resource.get("source"):
                collection["mh_source"] = resource["source"]
            elif "mh_source" in collection:
                del collection["mh_source"]
            collection["mh_unresolved"] = uid in plan["unresolved"]
            if resource:
                _set_properties(collection, resource.get("properties", {}))
            _link_to_scene(scene, collection, transaction)
            collections[uid] = collection

        imported_fbx = []
        warnings = list(plan["warnings"])
        for uid, resource in sorted(plan["resources"].items()):
            if resource["kind"] != "static_mesh":
                continue
            for slot in resource.get("material_slots", []):
                material_uid = slot["material_uid"]
                if material_uid not in plan["materials"]:
                    warnings.append(_warning(
                        "MH_W_MATERIAL_NOT_FOUND", [uid, material_uid],
                        f"mesh {uid} slot '{slot['slot_name']}' references "
                        f"absent material payload {material_uid}"))
        rehydrated_materials = set()
        if import_fbx:
            for uid, resource in sorted(plan["resources"].items()):
                source = resource.get("source")
                if resource["kind"] != "static_mesh" or not source \
                        or not os.path.isfile(source):
                    continue
                imported_objects, previous_objects = \
                    _import_fbx_into_collection(
                        scene, collections[uid], source, uid)
                retired.extend(
                    (collections[uid], obj) for obj in previous_objects)
                rehydrated_materials.update(_rehydrate_imported_materials(
                    uid, resource, imported_objects, plan["materials"],
                    existing_materials, transaction, warnings))
                imported_fbx.append(uid)

        # All target collections now exist, including unresolved empty
        # placeholders, so every instance link can be assigned deterministically.
        # Evaluate the newly linked hierarchy in its owning scene before restoring
        # the user's active scene; this makes matrix_world immediately reliable to
        # the exporter even when GEOMETRY was created in the background.
        window = bpy.context.window
        prior_scene = window.scene if window is not None else None
        try:
            if window is not None:
                window.scene = scene
            for uid, document in sorted(plan["documents"].items()):
                stale = _apply_document(
                    collections[uid], document, collections, transaction)
                retired.extend((collections[uid], obj) for obj in stale)
            if window is not None:
                bpy.context.view_layer.update()
        finally:
            if window is not None and prior_scene is not None:
                window.scene = prior_scene

        report = {
            "ok": True,
            "scene": scene,
            "root_collection": collections[plan["root_uid"]],
            "composite_uids": sorted(plan["documents"]),
            "mesh_uids": sorted(uid for uid, kind in kinds.items()
                                if kind == "static_mesh"),
            "imported_fbx": imported_fbx,
            "unresolved": plan["unresolved"],
            "warnings": sorted(
                warnings,
                key=lambda row: (
                    row["code"], row["subjects"], row["message"])),
            "rehydrated_materials": sorted(rehydrated_materials),
        }
        # Close the other half of the consumer race: an exporter may start
        # after the pre-mutation check while Blender is reading FBX payloads.
        # Reject that mixed snapshot and roll the whole Blender transaction
        # back before any previous payload objects are retired.
        _assert_plan_stable(plan)
        for collection, obj in retired:
            transaction.retire(collection, obj)
    except Exception:
        transaction.rollback()
        raise

    transaction.cleanup_retired([obj for _collection, obj in retired])
    transaction.cleanup_transport_ids()
    return report


def _texture_actualization_row(uid, name, slot, result):
    row = {
        "material_uid": uid,
        "material_name": name,
        "slot": slot,
        "basename": result.basename,
        "path": result.authored_path,
    }
    if result.code is not None:
        row["code"] = result.code
    if result.resolved_path is not None:
        row["resolved_path"] = result.resolved_path
    if result.candidates:
        row["candidates"] = list(result.candidates)
    return row


def _actualize_reachable_materials(
        source_root, material_uids, *, texture_policy="transitional",
        index_path=None, lock_root=None):
    """Repair stale paths for the preflight-reachable material UID set.

    This runs entirely before ``_BlenderImportTransaction`` exists. Each
    changed standalone payload is atomically replaced under its path lock.
    The disposable read index is rebuilt only after the source-edit wave.
    """
    root = os.path.normpath(os.path.abspath(source_root))
    texture_snapshot = capture_texture_tree(root)
    material_uids = tuple(sorted(set(material_uids)))
    result_report = {
        "materials_scanned": len(material_uids),
        "materials_updated": [],
        "fixed": [],
        "ambiguous": [],
        "missing": [],
        "exact": 0,
        "warnings": [],
    }

    for uid in material_uids:
        owner = _resolve_v2(
            root, uid, "material", index_path=index_path,
            lock_root=lock_root)
        if owner is None:
            _fail(
                "MH_E_UNRESOLVED_EXTERNAL",
                f"reachable material {uid} disappeared during texture "
                "actualization")
        token = capture_resource_stability_token(owner)
        try:
            document = _read_json(owner.payload_path)
        except CompositeImportError as exc:
            _fail(
                "MH_E_INVALID_MATERIAL_VALUE",
                f"cannot decode reachable material {uid}: {exc}")
        updated, results = actualize_material_document(
            document, texture_snapshot)
        fixes = [(slot, result) for slot, result in results
                 if result.status == "fixed"]

        if fixes:
            prepared = prepare_material_export(
                uid=updated["uid"],
                name=updated["name"],
                shader_class=updated["shader_class"],
                params=updated["params"],
                textures=updated["textures"],
                source_root=root,
                texture_policy=texture_policy,
                target_payload_path=owner.payload_path,
            )
            assert_resource_stable(token)
            assert_texture_tree_stable(texture_snapshot)

            def pre_replace_guard(current=token):
                # Re-prove identity and exact bytes while the destination's
                # per-path OS lock is held, immediately before replacement.
                assert_resource_stable(current)
                assert_texture_tree_stable(texture_snapshot)

            atomic_publish_json(
                prepared.payload_path, prepared.document,
                lock_root=lock_root, source_root=root,
                pre_replace_guard=pre_replace_guard)
            result_report["materials_updated"].append(uid)
        else:
            assert_resource_stable(token)
            assert_texture_tree_stable(texture_snapshot)

        for slot, result in results:
            if result.status == "exact":
                result_report["exact"] += 1
                continue
            row = _texture_actualization_row(
                uid, document["name"], slot, result)
            result_report[result.status].append(row)
            if result.code is not None:
                result_report["warnings"].append(_warning(
                    result.code, [uid],
                    f"material {uid} slot {slot} texture "
                    f"'{result.basename}': "
                    + ("multiple basename candidates; path unchanged"
                       if result.status == "ambiguous"
                       else "not found under Project Source Root")))

    assert_texture_tree_stable(texture_snapshot)
    if result_report["materials_updated"]:
        # Source edits invalidate every prior resolution/stability token.
        # Rebuild from payload authority; exporters never upsert this cache.
        rebuild_and_publish_index(
            root, fbx_passport_extractor=_fbx_passport_copies,
            index_path=index_path, lock_root=lock_root)
    return result_report


def import_composite_file(
        filepath, *, geometry_scene=None, import_fbx=True, source_root=None,
        texture_policy="transitional", index_path=None, lock_root=None):
    """Import a v2 clean-source graph with silent lazy index construction."""
    source_root = source_root or os.path.dirname(
        os.path.abspath(bpy.path.abspath(filepath)))
    normalized_root = os.path.abspath(
        bpy.path.abspath(os.fspath(source_root)))
    preflight_plan = load_composite_plan(
        filepath, source_root=normalized_root,
        texture_policy=texture_policy, index_path=index_path,
        lock_root=lock_root)
    # Destination conflicts are rejected before automatic source edits.
    _preflight_destination(preflight_plan)
    _assert_plan_stable(preflight_plan)
    texture_actualization = _actualize_reachable_materials(
        normalized_root, preflight_plan["materials"],
        texture_policy=texture_policy, index_path=index_path,
        lock_root=lock_root)

    # The plan is deliberately discarded after the possible source edit.
    # import_composite resolves again from the newly rebuilt read-side cache.
    report = import_composite(
        filepath, geometry_scene=geometry_scene, import_fbx=import_fbx,
        source_root=normalized_root, texture_policy=texture_policy,
        index_path=index_path, lock_root=lock_root)
    report["texture_actualization"] = texture_actualization
    if texture_actualization["warnings"]:
        report["warnings"] = sorted(
            report["warnings"] + texture_actualization["warnings"],
            key=lambda row: (
                row["code"], row["subjects"], row["message"]))
    report["imported_meshes"] = report["mesh_uids"]
    report["imported_composites"] = report["composite_uids"]
    report["placeholders"] = report["unresolved"]
    return report
