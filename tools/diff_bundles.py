"""Reader-side v2 source snapshot differ.

Usage::

    python tools/diff_bundles.py OLD_SOURCE_DIR NEW_SOURCE_DIR \
        [--old-rel SOURCE_REL] [--new-rel SOURCE_REL]

The historical filename is retained for automation compatibility.  The tool
recursively reads only Clean Sources v2 primary payloads:

* ``*.mesh.fbx`` through the production Carrier B passport reader;
* ``*.composite`` schema version 2;
* ``*.material`` schema version 1.

It never reads ``export_manifest.json`` and never writes a cache/index.  Diff
classification is entirely reader-side.  Snapshots containing FBX must run in
an environment where the add-on's production ``io_scene_fbx`` reader is
available; JSON-only snapshots remain ordinary-CPython compatible.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
import json
import math
import os
from pathlib import Path
import posixpath
import sys
import unicodedata
import uuid

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(TOOLS_DIR), "addon"))

from mh4blend.core.canonical import validate_resource_name  # noqa: E402
from mh4blend.core.diff import diff_source_snapshots  # noqa: E402
from mh4blend.core.fbx_passport import read_fbx_passport  # noqa: E402
from mh4blend.core.material_source import (  # noqa: E402
    MaterialSourceError,
    validate_material_document,
)

__all__ = ["SourceSnapshotError", "load_source_snapshot", "main"]


_PRIMARY_SUFFIXES = (".mesh.fbx", ".composite", ".material")
_COMPOSITE_FIELDS = frozenset({
    "schema", "schema_version", "uid", "name", "properties", "nodes",
})
_NODE_BASE_FIELDS = frozenset({
    "node_uid", "parent_uid", "kind", "display_name", "local_transform",
    "properties",
})
_TRANSFORM_FIELDS = frozenset({
    "translation_cm", "rotation_quat", "scale",
})


class SourceSnapshotError(RuntimeError):
    """A source directory cannot form one unambiguous v2 snapshot."""

    def __init__(self, code, message):
        self.code = code
        super().__init__(f"{code}: {message}")


def _fail(code, message):
    raise SourceSnapshotError(code, message)


def _canonical_uuid(value, field):
    if not isinstance(value, str):
        _fail("MH_E_INVALID_COMPOSITE", f"{field} must be a canonical UUID")
    try:
        canonical = str(uuid.UUID(value))
    except (ValueError, AttributeError, TypeError) as exc:
        _fail("MH_E_INVALID_COMPOSITE", f"{field}: {exc}")
    if value != canonical:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"{field} must use lowercase canonical UUID spelling")
    return canonical


def _strict_json_bytes(payload, path):
    def reject_constant(value):
        raise ValueError(f"non-finite JSON constant {value}")

    try:
        return json.loads(
            payload.decode("utf-8"), parse_constant=reject_constant)
    except (UnicodeError, json.JSONDecodeError, ValueError) as exc:
        _fail("MH_E_PASSPORT_INVALID", f"cannot parse {path}: {exc}")


def _stat_token(stat_result):
    return (
        int(stat_result.st_size), int(stat_result.st_mtime_ns),
        int(getattr(stat_result, "st_ctime_ns", 0)),
    )


def _read_stable_bytes(path):
    try:
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno())
            payload = stream.read()
            after = os.fstat(stream.fileno())
        current = path.stat()
    except OSError as exc:
        _fail(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"cannot read stable payload {path}: {exc}")
    if (_stat_token(before) != _stat_token(after)
            or (after.st_size, after.st_mtime_ns)
            != (current.st_size, current.st_mtime_ns)
            or len(payload) != current.st_size):
        _fail(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"payload changed while reading: {path}")
    return payload


def _number(value, subject):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail("MH_E_INVALID_COMPOSITE", f"{subject} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        _fail("MH_E_NAN_INF_VALUE", f"{subject} is NaN/Inf")
    return result


def _vector(value, count, subject):
    if not isinstance(value, list) or len(value) != count:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"{subject} must be an array of {count} numbers")
    return tuple(_number(component, subject) for component in value)


def _validate_transform(value, node_uid):
    if not isinstance(value, dict) or set(value) != _TRANSFORM_FIELDS:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"node {node_uid} local_transform has invalid fields")
    translation = _vector(
        value["translation_cm"], 3, f"node {node_uid} translation_cm")
    rotation = _vector(
        value["rotation_quat"], 4, f"node {node_uid} rotation_quat")
    scale = _vector(value["scale"], 3, f"node {node_uid} scale")
    if sum(component * component for component in rotation) == 0.0:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"node {node_uid} has a zero quaternion")
    if any(component <= 0.0 for component in scale):
        _fail("MH_E_INVALID_SCALE", f"node {node_uid} scale is {scale}")
    return {
        "translation_cm": list(translation),
        "rotation_quat": list(rotation),
        "scale": list(scale),
    }


def _validate_composite_v2(document, path):
    if not isinstance(document, dict):
        _fail("MH_E_INVALID_COMPOSITE", f"{path} root must be an object")
    if document.get("schema") != "mh.composite":
        _fail("MH_E_INVALID_COMPOSITE", f"{path} is not mh.composite")
    version = document.get("schema_version")
    if isinstance(version, bool) or not isinstance(version, int) or version != 2:
        if version == 1:
            _fail(
                "MH_E_UNKNOWN_SCHEMA_VERSION",
                "composite v1 is not a production candidate; "
                "MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED")
        _fail(
            "MH_E_UNKNOWN_SCHEMA_VERSION",
            f"unsupported composite schema_version {version!r} in {path}")
    if set(document) != _COMPOSITE_FIELDS:
        _fail(
            "MH_E_INVALID_COMPOSITE",
            f"{path} has invalid fields; "
            f"missing={sorted(_COMPOSITE_FIELDS - set(document))}, "
            f"unknown={sorted(set(document) - _COMPOSITE_FIELDS)}")
    uid = _canonical_uuid(document["uid"], "composite.uid")
    name = document["name"]
    try:
        validate_resource_name(name)
    except (TypeError, ValueError) as exc:
        _fail("MH_E_NON_ASCII_RESOURCE_NAME", f"{path}: {exc}")
    if not isinstance(document["properties"], dict):
        _fail("MH_E_INVALID_COMPOSITE", f"{path} properties must be an object")
    rows = document["nodes"]
    if not isinstance(rows, list):
        _fail("MH_E_INVALID_COMPOSITE", f"{path} nodes must be an array")

    normalized_nodes = []
    by_uid = {}
    for raw in rows:
        if not isinstance(raw, dict):
            _fail("MH_E_INVALID_COMPOSITE", f"{path} node must be an object")
        kind = raw.get("kind")
        if kind not in {"group", "mesh", "composite_ref"}:
            _fail(
                "MH_E_UNSUPPORTED_NODE_KIND",
                f"unsupported node kind {kind!r} in {path}")
        expected = _NODE_BASE_FIELDS if kind == "group" else \
            _NODE_BASE_FIELDS | {"resource_uid"}
        if set(raw) != expected:
            _fail(
                "MH_E_INVALID_COMPOSITE",
                f"{path} node has invalid fields; "
                f"missing={sorted(expected - set(raw))}, "
                f"unknown={sorted(set(raw) - expected)}")
        node_uid = _canonical_uuid(raw["node_uid"], "node_uid")
        if node_uid in by_uid:
            _fail("MH_E_DUPLICATE_NODE_UID", f"duplicate node {node_uid}")
        parent_uid = raw["parent_uid"]
        if parent_uid is not None:
            parent_uid = _canonical_uuid(parent_uid, "parent_uid")
        display_name = raw["display_name"]
        if not isinstance(display_name, str) or not display_name:
            _fail(
                "MH_E_INVALID_COMPOSITE",
                f"node {node_uid} display_name must be non-empty text")
        if not isinstance(raw["properties"], dict):
            _fail(
                "MH_E_INVALID_COMPOSITE",
                f"node {node_uid} properties must be an object")
        normalized = {
            "node_uid": node_uid,
            "parent_uid": parent_uid,
            "kind": kind,
            "display_name": unicodedata.normalize("NFC", display_name),
            "local_transform": _validate_transform(
                raw["local_transform"], node_uid),
            "properties": deepcopy(raw["properties"]),
        }
        if kind != "group":
            normalized["resource_uid"] = _canonical_uuid(
                raw["resource_uid"], "resource_uid")
        normalized_nodes.append(normalized)
        by_uid[node_uid] = normalized

    for node in normalized_nodes:
        parent_uid = node["parent_uid"]
        if parent_uid is not None and parent_uid not in by_uid:
            _fail(
                "MH_E_DANGLING_PARENT",
                f"node {node['node_uid']} references {parent_uid}")
    for node in normalized_nodes:
        visited = set()
        cursor = node
        while cursor["parent_uid"] is not None:
            if cursor["node_uid"] in visited:
                _fail(
                    "MH_E_PARENT_CYCLE",
                    f"parent cycle containing {sorted(visited)}")
            visited.add(cursor["node_uid"])
            cursor = by_uid[cursor["parent_uid"]]

    return {
        "schema": "mh.composite",
        "schema_version": 2,
        "uid": uid,
        "name": unicodedata.normalize("NFC", name),
        "properties": deepcopy(document["properties"]),
        "nodes": normalized_nodes,
    }


def _relative_source_path(root, path, root_rel):
    relative = path.relative_to(root).as_posix()
    prefix = posixpath.normpath((root_rel or ".").replace("\\", "/"))
    if (posixpath.isabs(prefix) or prefix == ".."
            or prefix.startswith("../")):
        raise ValueError("snapshot relative root must stay below source_root")
    return relative if prefix == "." else posixpath.join(prefix, relative)


def _is_reserved_temp(filename):
    return ".mh-tmp-" in filename or ".writing." in filename


def _discover(root):
    discovered = {}
    for directory, dirnames, filenames in os.walk(root, followlinks=False):
        dirnames.sort(key=lambda item: (item.casefold(), item))
        filenames.sort(key=lambda item: (item.casefold(), item))
        for filename in filenames:
            lower = filename.lower()
            if (_is_reserved_temp(filename)
                    or not lower.endswith(_PRIMARY_SUFFIXES)):
                continue
            path = Path(directory, filename)
            try:
                resolved = path.resolve(strict=True)
                resolved.relative_to(root)
            except (OSError, ValueError) as exc:
                _fail(
                    "MH_E_SOURCE_INDEX_PATH_OUTSIDE_ROOT",
                    f"payload escapes source root: {path}: {exc}")
            if not resolved.is_file():
                continue
            prior = discovered.get(resolved)
            if prior is not None and prior != path:
                _fail(
                    "MH_E_SOURCE_INDEX_INVALID",
                    f"payload paths alias the same file: {prior}, {path}")
            discovered[resolved] = path
    return tuple(sorted(discovered, key=lambda item: item.as_posix()))


def _read_fbx_row(path, source_path, fbx_reader):
    try:
        before = _stat_token(path.stat())
        payload = _read_stable_bytes(path)
        before_reader = _stat_token(path.stat())
        if before != before_reader:
            _fail(
                "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
                f"FBX changed before passport read: {path}")
        receipt = fbx_reader(path)
        after = _stat_token(path.stat())
    except SourceSnapshotError:
        raise
    except Exception as exc:
        _fail("MH_E_PASSPORT_INVALID", f"cannot read {path}: {exc}")
    if before != after:
        _fail(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"FBX changed during passport read: {path}")
    document = getattr(receipt, "document", None)
    descriptor = getattr(receipt, "descriptor_hash", None)
    if (not isinstance(document, dict)
            or document.get("schema") != "mh.fbx_passport"
            or not isinstance(descriptor, str)):
        _fail("MH_E_PASSPORT_INVALID", f"invalid passport receipt for {path}")
    return {
        "uid": document["resource_uid"],
        "kind": "static_mesh",
        "name": document["name"],
        "source_path": source_path,
        "payload_fingerprint": "sha256:" + hashlib.sha256(payload).hexdigest(),
        "geometry_hash": document["geometry_hash"],
        "descriptor_hash": descriptor,
        "document": deepcopy(document),
    }


def _read_json_row(path, source_path, root):
    payload = _read_stable_bytes(path)
    fingerprint = "sha256:" + hashlib.sha256(payload).hexdigest()
    document = _strict_json_bytes(payload, path)
    if path.name.lower().endswith(".material"):
        try:
            normalized, _diagnostics = validate_material_document(
                document, source_root=str(root), texture_policy="transitional")
        except MaterialSourceError as exc:
            _fail(exc.code, f"{path}: {exc}")
        return {
            "uid": normalized["uid"],
            "kind": "material",
            "name": normalized["name"],
            "source_path": source_path,
            "payload_fingerprint": fingerprint,
            "document": normalized,
        }
    normalized = _validate_composite_v2(document, path)
    return {
        "uid": normalized["uid"],
        "kind": "composite",
        "name": normalized["name"],
        "source_path": source_path,
        "payload_fingerprint": fingerprint,
        "document": normalized,
    }


def load_source_snapshot(source_dir, *, root_rel="", fbx_reader=None):
    """Read one stable v2 payload directory into an in-memory snapshot.

    ``fbx_reader`` exists for pure tests; production calls always default to
    :func:`mh4blend.core.fbx_passport.read_fbx_passport`.
    """
    root = Path(source_dir).resolve()
    if not root.is_dir():
        _fail("MH_E_SOURCE_INDEX_INVALID", f"not a source directory: {root}")
    fbx_reader = fbx_reader or read_fbx_passport
    first_inventory = _discover(root)
    resources = {}
    diagnostics = []
    for path in first_inventory:
        source_path = _relative_source_path(root, path, root_rel)
        if path.name.lower().endswith(".mesh.fbx"):
            row = _read_fbx_row(path, source_path, fbx_reader)
        else:
            row = _read_json_row(path, source_path, root)
        uid = row["uid"]
        if uid in resources:
            prior = resources[uid]
            paths = sorted((prior["source_path"], source_path))
            if (prior.get("payload_fingerprint")
                    != row.get("payload_fingerprint")):
                _fail(
                    "MH_E_DIVERGENT_REVISIONS",
                    f"snapshot has divergent candidates for {uid}: "
                    + ", ".join(paths))
            diagnostics.append(
                "MH_W_DUPLICATE_IDENTICAL_PAYLOAD: " + ", ".join(paths))
            if source_path < prior["source_path"]:
                resources[uid] = row
        else:
            resources[uid] = row
    if _discover(root) != first_inventory:
        _fail(
            "MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED",
            f"source payload set changed during scan: {root}")
    return {
        "schema": "mh.source_snapshot",
        "schema_version": 1,
        "root": str(root),
        "resources": dict(sorted(resources.items())),
        "diagnostics": sorted(set(diagnostics)),
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("old_source")
    parser.add_argument("new_source")
    parser.add_argument("--old-rel", default="")
    parser.add_argument("--new-rel", default="")
    args = parser.parse_args(argv)

    try:
        old_snapshot = load_source_snapshot(
            args.old_source, root_rel=args.old_rel)
        new_snapshot = load_source_snapshot(
            args.new_source, root_rel=args.new_rel)
        report = diff_source_snapshots(old_snapshot, new_snapshot)
    except (SourceSnapshotError, ValueError, TypeError) as exc:
        raise SystemExit(str(exc)) from exc
    json.dump(report, sys.stdout, indent=2, ensure_ascii=False)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
