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

import bpy
from mathutils import Matrix, Quaternion, Vector

from ..core.canonical import ERROR_CODES, validate_resource_name
from ..core.materials import material_disk_payload
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
from .source_manifest import (
    ManifestError,
    assert_manifest_stable,
    load_export_manifest_snapshot,
    manifest_resource_index,
    validate_export_manifest,
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
        for obj in objects:
            if obj.name in bpy.data.objects and not obj.users_collection:
                with contextlib.suppress(RuntimeError, ReferenceError):
                    bpy.data.objects.remove(obj, do_unlink=True)

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


def _read_json(path):
    try:
        with open(path, encoding="utf-8") as stream:
            return json.load(stream)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        _fail("MH_E_INVALID_COMPOSITE", f"cannot read '{path}': {exc}")


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


def _validate_transform(value, node_uid):
    if not isinstance(value, dict):
        _fail("MH_E_INVALID_COMPOSITE",
              f"node {node_uid} local_transform must be an object")
    translation = _vector(
        value.get("translation_cm"), 3,
        f"node {node_uid} translation_cm")
    rotation = _vector(
        value.get("rotation_quat"), 4,
        f"node {node_uid} rotation_quat")
    scale = _vector(value.get("scale"), 3, f"node {node_uid} scale")
    if sum(component * component for component in rotation) == 0.0:
        _fail("MH_E_INVALID_COMPOSITE",
              f"node {node_uid} has a zero quaternion")
    if any(component <= 0.0 for component in scale):
        _fail("MH_E_INVALID_SCALE", f"node {node_uid} scale is {scale}")
    return {
        "translation_cm": translation,
        "rotation_quat": rotation,
        "scale": scale,
    }


def _validate_document(document, path):
    if not isinstance(document, dict):
        _fail("MH_E_INVALID_COMPOSITE", f"'{path}' is not a JSON object")
    if document.get("schema") != "mh.composite" \
            or document.get("schema_version") != 1:
        _fail("MH_E_UNKNOWN_SCHEMA_VERSION",
              f"unsupported composite schema in '{path}'")
    uid = document.get("uid")
    name = document.get("name")
    if not isinstance(uid, str) or not uid:
        _fail("MH_E_INVALID_COMPOSITE", f"'{path}' has no uid")
    if not isinstance(name, str) or not name:
        _fail("MH_E_INVALID_COMPOSITE", f"'{path}' has no name")
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        _fail("MH_E_NON_ASCII_RESOURCE_NAME", f"'{path}': {exc}")
    rows = document.get("nodes")
    if not isinstance(rows, list):
        _fail("MH_E_INVALID_COMPOSITE", f"'{path}' nodes must be an array")

    nodes = []
    by_uid = {}
    for raw in rows:
        if not isinstance(raw, dict):
            _fail("MH_E_INVALID_COMPOSITE", f"'{path}' node is not an object")
        node_uid = raw.get("node_uid")
        if not isinstance(node_uid, str) or not node_uid:
            _fail("MH_E_INVALID_COMPOSITE", f"'{path}' node has no node_uid")
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
        display_name = raw.get("display_name")
        if not isinstance(display_name, str) or not display_name:
            _fail("MH_E_INVALID_COMPOSITE",
                  f"node {node_uid} has no display_name")
        parent_uid = raw.get("parent_uid")
        if parent_uid is not None and (not isinstance(parent_uid, str)
                                       or not parent_uid):
            _fail("MH_E_INVALID_COMPOSITE",
                  f"node {node_uid} has invalid parent_uid")
        resource_uid = raw.get("resource_uid")
        if kind == "group":
            if resource_uid is not None:
                _fail("MH_E_INVALID_COMPOSITE",
                      f"group node {node_uid} must not have resource_uid")
        elif not isinstance(resource_uid, str) or not resource_uid:
            _fail("MH_E_INVALID_COMPOSITE",
                  f"node {node_uid} kind {kind} requires resource_uid")
        properties = raw.get("properties", {})
        if not isinstance(properties, dict):
            _fail("MH_E_INVALID_COMPOSITE",
                  f"node {node_uid} properties must be an object")
        normalized = {
            "node_uid": node_uid,
            "parent_uid": parent_uid,
            "kind": kind,
            "display_name": display_name,
            "resource_uid": resource_uid,
            "local_transform": _validate_transform(
                raw.get("local_transform"), node_uid),
            "properties": deepcopy(properties),
            # Preserve forward-compatible, non-semantic source metadata for
            # inspection. It is not interpreted as random/variant behavior.
            "custom_metadata": {
                key: deepcopy(value) for key, value in raw.items()
                if key not in _NODE_FIELDS
            },
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
    return {"uid": uid, "name": name, "nodes": nodes, "path": path}


def _safe_source(base_directory, source, uid):
    if not isinstance(source, str) or not source:
        return None
    if os.path.isabs(source) or (len(source) > 1 and source[1] == ":"):
        _fail("MH_E_INVALID_RESOURCE_SOURCE",
              f"resource {uid} has absolute source '{source}'")
    base = os.path.realpath(os.path.abspath(base_directory))
    target = os.path.realpath(os.path.abspath(os.path.join(base, source)))
    try:
        inside = os.path.commonpath(
            [os.path.normcase(base), os.path.normcase(target)]) \
            == os.path.normcase(base)
    except ValueError:
        inside = False
    if not inside or target == base:
        _fail("MH_E_INVALID_RESOURCE_SOURCE",
              f"resource {uid} escapes the import directory")
    return target


def _load_manifest(root_directory, manifest):
    if manifest is None:
        try:
            document, token = load_export_manifest_snapshot(
                root_directory, allow_missing=True)
        except ManifestError as exc:
            _fail("MH_E_INVALID_EXPORT_MANIFEST", str(exc))
        return document, root_directory, token, True
    if isinstance(manifest, dict):
        try:
            return (validate_export_manifest(manifest), root_directory,
                    None, False)
        except ManifestError as exc:
            _fail("MH_E_INVALID_EXPORT_MANIFEST", str(exc))
    manifest_path = bpy.path.abspath(manifest)
    directory = manifest_path if os.path.isdir(manifest_path) \
        else os.path.dirname(manifest_path)
    try:
        document, token = load_export_manifest_snapshot(directory)
        return document, directory, token, True
    except ManifestError as exc:
        _fail("MH_E_INVALID_EXPORT_MANIFEST", str(exc))


def load_composite_plan(filepath, manifest=None, resource_resolver=None):
    """Read and preflight a root composite plus every reachable local doc.

    A resolver may return a manifest-style row for a UID not present in the
    sibling manifest. Missing files/resources are legal unresolved placeholders.
    No Blender datablock is created or changed by this function.
    """
    root_path = os.path.abspath(bpy.path.abspath(filepath))
    root_directory = os.path.dirname(root_path)
    (manifest_doc, manifest_directory, manifest_token,
     check_manifest_stability) = _load_manifest(root_directory, manifest)
    index = manifest_resource_index(manifest_doc) if manifest_doc else {}
    materials = {}
    for row in (manifest_doc or {}).get("materials", []):
        uid = row["uid"]  # source_manifest already validates identity/duplicates
        if row.get("kind") != "material":
            _fail("MH_E_RESOURCE_KIND_MISMATCH",
                  f"manifest material {uid} has kind {row.get('kind')!r}")
        if not isinstance(row.get("name"), str) or not row["name"]:
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"manifest material {uid} has no name")
        if not isinstance(row.get("shader_class"), str):
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"manifest material {uid} has invalid shader_class")
        if not isinstance(row.get("textures", {}), dict):
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"manifest material {uid} textures must be an object")
        params = row.get("params", row.get("optional", {}))
        if not isinstance(params, dict):
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"manifest material {uid} params must be an object")
        if "optional" in row and not isinstance(row["optional"], dict):
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"manifest material {uid} optional must be an object")
        materials[uid] = deepcopy(row)

    def resolve(uid, expected_kind):
        row = index.get(uid)
        if row is None and resource_resolver is not None:
            row = resource_resolver(uid, expected_kind)
        if row is None:
            return None
        if not isinstance(row, dict):
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"resolver row for {uid} is not an object")
        actual_kind = row.get("kind")
        if actual_kind != expected_kind:
            _fail("MH_E_RESOURCE_KIND_MISMATCH",
                  f"resource {uid} is {actual_kind!r}, expected "
                  f"{expected_kind!r}")
        return deepcopy(row)

    root = _validate_document(_read_json(root_path), root_path)
    documents = {root["uid"]: root}
    document_paths = {root["uid"]: root_path}
    resources = {}
    unresolved = set()
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
            row = resolve(uid, expected)
            source = None
            if row is not None:
                source = _safe_source(
                    manifest_directory, row.get("source"), uid)
                properties = row.get("properties", {})
                if not isinstance(properties, dict):
                    _fail("MH_E_INVALID_EXPORT_MANIFEST",
                          f"resource {uid} properties must be an object")
                row_name = row.get("name")
                if not isinstance(row_name, str) or not row_name:
                    _fail("MH_E_INVALID_EXPORT_MANIFEST",
                          f"resource {uid} has no name")
                try:
                    validate_resource_name(row_name)
                except (TypeError, ValueError) as exc:
                    _fail("MH_E_NON_ASCII_RESOURCE_NAME",
                          f"resource {uid}: {exc}")
                material_slots = row.get("material_slots", [])
                if expected == "static_mesh":
                    if not isinstance(material_slots, list):
                        _fail("MH_E_INVALID_EXPORT_MANIFEST",
                              f"resource {uid} material_slots must be an array")
                    seen_slot_names = {}
                    for slot in material_slots:
                        if not isinstance(slot, dict):
                            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                                  f"resource {uid} material slot is not an object")
                        slot_name = slot.get("slot_name")
                        material_uid = slot.get("material_uid")
                        if not isinstance(slot_name, str) or not slot_name \
                                or not isinstance(material_uid, str) \
                                or not material_uid:
                            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                                  f"resource {uid} has an invalid material slot")
                        prior_material_uid = seen_slot_names.get(slot_name)
                        if prior_material_uid is not None \
                                and prior_material_uid != material_uid:
                            _fail("MH_E_MATERIAL_SLOT_CONFLICT",
                                  f"resource {uid} slot {slot_name!r} maps to "
                                  "multiple material UIDs")
                        seen_slot_names[slot_name] = material_uid
            resources[uid] = {
                "uid": uid,
                "kind": expected,
                "name": (row or {}).get("name") or node["display_name"],
                "source": source,
                "properties": deepcopy((row or {}).get("properties", {})),
                "material_slots": deepcopy(
                    (row or {}).get("material_slots", []))
                    if expected == "static_mesh" else [],
            }
            if expected == "composite":
                if source is None or not os.path.isfile(source):
                    unresolved.add(uid)
                    continue
                loaded = _validate_document(_read_json(source), source)
                if loaded["uid"] != uid:
                    _fail("MH_E_RESOURCE_UID_MISMATCH",
                          f"'{source}' uid {loaded['uid']} != manifest uid {uid}")
                if loaded["name"] != row["name"]:
                    _fail("MH_E_NAME_MISMATCH",
                          f"'{source}' name {loaded['name']!r} != manifest "
                          f"name {row['name']!r}")
                if uid in documents:
                    if os.path.normcase(document_paths[uid]) != \
                            os.path.normcase(source):
                        _fail("MH_E_DUPLICATE_RESOURCE_UID",
                              f"composite {uid} resolves to multiple files")
                else:
                    documents[uid] = loaded
                    document_paths[uid] = source
                    queue.append(loaded)
            elif source is None or not os.path.isfile(source):
                unresolved.add(uid)

    # The selected root path is authoritative, while its optional manifest row
    # still contributes resource-level properties shown on the collection.
    if root["uid"] in resources \
            and resources[root["uid"]]["kind"] != "composite":
        _fail("MH_E_RESOURCE_KIND_MISMATCH",
              f"root composite {root['uid']} is also referenced as a mesh")
    root_row = resolve(root["uid"], "composite")
    root_properties = {}
    if root_row is not None:
        root_properties = root_row.get("properties", {})
        if not isinstance(root_properties, dict):
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"resource {root['uid']} properties must be an object")
        root_name = root_row.get("name")
        if not isinstance(root_name, str) or not root_name:
            _fail("MH_E_INVALID_EXPORT_MANIFEST",
                  f"resource {root['uid']} has no name")
        try:
            validate_resource_name(root_name)
        except (TypeError, ValueError) as exc:
            _fail("MH_E_NON_ASCII_RESOURCE_NAME",
                  f"resource {root['uid']}: {exc}")
        if root_name != root["name"]:
            _fail("MH_E_NAME_MISMATCH",
                  f"selected composite name {root['name']!r} != manifest "
                  f"name {root_name!r}")
        # Validate containment even though the explicitly selected file wins.
        _safe_source(manifest_directory, root_row.get("source"), root["uid"])
    resources[root["uid"]] = {
        "uid": root["uid"], "kind": "composite", "name": root["name"],
        "source": root_path, "properties": deepcopy(root_properties),
    }

    graph = {uid: [] for uid in documents}
    for uid, document in documents.items():
        graph[uid] = [
            node["resource_uid"] for node in document["nodes"]
            if node["kind"] == "composite_ref"
            and node["resource_uid"] in documents
        ]
    colors = {uid: 0 for uid in graph}

    def visit(uid, stack):
        colors[uid] = 1
        stack.append(uid)
        for target_uid in graph[uid]:
            if colors[target_uid] == 1:
                cycle = stack[stack.index(target_uid):] + [target_uid]
                _fail("MH_E_COMPOSITE_CYCLE", " -> ".join(cycle))
            if colors[target_uid] == 0:
                visit(target_uid, stack)
        stack.pop()
        colors[uid] = 2

    for uid in sorted(graph):
        if colors[uid] == 0:
            visit(uid, [])

    return {
        "root_uid": root["uid"],
        "documents": documents,
        "resources": resources,
        "unresolved": sorted(unresolved),
        "manifest": manifest_doc,
        "materials": materials,
        "manifest_directory": manifest_directory,
        "manifest_token": manifest_token,
        "check_manifest_stability": check_manifest_stability,
    }


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
    is exactly the canonical manifest payload. On any unsupported value or
    partial write, the prior RNA state is restored and its shader is cleared,
    leaving the lossless manifest JSON fallback authoritative.

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
            f"mesh {resource_uid} FBX slot '{slot_name}' has no manifest "
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
        obj.empty_display_type = "PLAIN_AXES"
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
            obj.instance_type = "COLLECTION"
            obj.instance_collection = collections[node["resource_uid"]]
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


def import_composite(filepath, manifest=None, geometry_scene=None,
                     resource_resolver=None, import_fbx=True):
    """Import a dependency graph into collection definitions in GEOMETRY.

    Existing collections with the same composite UID are updated in place.
    Invalid documents, dependency cycles, and destination UID conflicts are
    rejected during preflight, before any scene/datablock mutation.
    """
    plan = load_composite_plan(filepath, manifest, resource_resolver)
    kinds, names, existing, existing_materials = _preflight_destination(plan)
    transaction = _BlenderImportTransaction()
    if plan["check_manifest_stability"]:
        try:
            assert_manifest_stable(
                plan["manifest_directory"], plan["manifest_token"])
        except ManifestError as exc:
            _fail("MH_E_INVALID_EXPORT_MANIFEST", str(exc))
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
        warnings = []
        for uid, resource in sorted(plan["resources"].items()):
            if resource["kind"] != "static_mesh":
                continue
            for slot in resource.get("material_slots", []):
                material_uid = slot["material_uid"]
                if material_uid not in plan["materials"]:
                    warnings.append(_warning(
                        "MH_W_MATERIAL_NOT_FOUND", [uid, material_uid],
                        f"mesh {uid} slot '{slot['slot_name']}' references "
                        f"absent manifest material {material_uid}"))
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
        if plan["check_manifest_stability"]:
            try:
                assert_manifest_stable(
                    plan["manifest_directory"], plan["manifest_token"])
            except ManifestError as exc:
                _fail("MH_E_INVALID_EXPORT_MANIFEST", str(exc))
        for collection, obj in retired:
            transaction.retire(collection, obj)
    except Exception:
        transaction.rollback()
        raise

    transaction.cleanup_retired([obj for _collection, obj in retired])
    transaction.cleanup_transport_ids()
    return report


def import_composite_file(filepath, manifest_path=None, **kwargs):
    """UI-facing alias using a path to an optional export manifest."""
    report = import_composite(filepath, manifest=manifest_path, **kwargs)
    report["imported_meshes"] = report["mesh_uids"]
    report["imported_composites"] = report["composite_uids"]
    report["placeholders"] = report["unresolved"]
    return report
