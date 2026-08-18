"""Reproducible G1 spike for the proposed ``mh.fbx_passport:1`` carrier.

Run with Blender 4.5+ (not ordinary CPython)::

    blender --background --factory-startup --python \
      tools/spikes/fbx_passport_transport.py -- --outdir dist/spikes/g1-passport

This is deliberately isolated from the add-on.  It writes real binary FBX
files with Blender's Python FBX exporter, then reads each file through both
Blender 4.5 import paths: ``import_scene.fbx`` (io_scene_fbx) and
``wm.fbx_import`` (native ufbx).  The output JSON is a measurement receipt,
not a production protocol codec.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import sys
import time
import unicodedata
import uuid

import bpy
from mathutils import Vector


PASSPORT_PROPERTY = "mh_fbx_passport"
LOD_LEVEL_PROPERTY = "mh_lod_level"
SENTINEL_NAME = "MH_PASSPORT_SENTINEL"
SCHEMA = "mh.fbx_passport"
SCHEMA_VERSION = 1


def canonical_passport(document: dict) -> str:
    """Return the spike's canonical, UTF-8-friendly, one-line JSON value."""

    def nfc(value):
        if isinstance(value, str):
            return unicodedata.normalize("NFC", value)
        if isinstance(value, list):
            return [nfc(item) for item in value]
        if isinstance(value, dict):
            normalized = {nfc(key): nfc(item) for key, item in value.items()}
            if len(normalized) != len(value):
                raise ValueError("passport keys collide after Unicode NFC")
            return normalized
        return value

    value = json.dumps(
        nfc(document),
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    )
    if "\n" in value or "\r" in value:
        raise AssertionError("passport must occupy one custom-property line")
    return value


def passport_document(
        *, name: str, lod_levels: list[int] | None = None,
        long_probe_bytes: int = 0) -> dict:
    # The composed e-acute is intentional: this proves UTF-8/NFC transport
    # rather than merely ASCII JSON transport.
    properties = {"role": "g1_probe", "unicode_nfc_probe": "Гараж_é"}
    if long_probe_bytes:
        properties["long_probe"] = "x" * long_probe_bytes
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "resource_uid": str(uuid.uuid5(uuid.NAMESPACE_URL, f"mh-g1:{name}")),
        "kind": "static_mesh",
        "name": name,
        "lod_levels": [0] if lod_levels is None else list(lod_levels),
        "lod_policy": "authored",
        "geometry_hash": "xxh3:g100000000000000",
        "material_slots": [{
            "slot_name": "g1_surface",
            "material_uid": str(uuid.uuid5(
                uuid.NAMESPACE_URL, "mh-g1:material:g1_surface")),
            "material_name_hint": "m_g1_surface",
        }],
        "properties": properties,
        "exporter": "mh4blend G1 spike",
    }


def _clear_scene() -> None:
    # Blender 4.5 native ufbx imports shared FBX geometry with an under-counted
    # Mesh.users value (two object references, users==1).  Deleting that graph
    # produces an internal ID decrement error.  Retain only those broken probe
    # objects until process exit, rename them out of the way, and remove every
    # healthy object normally.  This also makes the defect visible in receipts.
    mesh_refs = {}
    for obj in bpy.data.objects:
        if obj.type == "MESH" and obj.data is not None:
            mesh_refs.setdefault(obj.data, 0)
            mesh_refs[obj.data] += 1
    broken_meshes = {
        mesh for mesh, refs in mesh_refs.items() if refs > mesh.users
    }
    retained = {
        obj for obj in bpy.data.objects
        if obj.type == "MESH" and obj.data in broken_meshes
    }
    for index, obj in enumerate(sorted(retained, key=lambda item: item.name)):
        if not obj.name.startswith("__G1_UFBX_RETAINED_"):
            obj.name = f"__G1_UFBX_RETAINED_{index}_{obj.name}"
        if obj.name not in bpy.context.scene.collection.objects:
            bpy.context.scene.collection.objects.link(obj)
        for collection in list(obj.users_collection):
            if collection != bpy.context.scene.collection:
                collection.objects.unlink(obj)
    # Batch removal lets Blender resolve shared datablock users as one graph;
    # removing shared objects one-by-one can emit a false ID user-count error.
    bpy.data.batch_remove(ids=(
        *(obj for obj in bpy.data.objects if obj not in retained),
        *tuple(bpy.data.collections),
    ))
    # Do not eagerly purge mesh datablocks.  Blender's FBX importers may retain
    # short-lived internal references after object removal; orphan datablocks
    # are harmless in this short-lived spike process, while forcing removal can
    # trigger a Blender ID user-count diagnostic for shared datablocks.


def _triangle_mesh(name: str, *, custom_normals: bool = False):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.2), (0.0, 1.0, 0.0)],
        [],
        [(0, 1, 2)],
    )
    mesh.update()
    if custom_normals:
        # Deliberately non-face normals, normalized and one per corner.
        normals = [Vector((0.15, 0.25, 0.956556)).normalized()] * 3
        mesh.normals_split_custom_set(normals)
        if not mesh.has_custom_normals:
            raise AssertionError("failed to create the custom-normal probe")
    return mesh


def _link_object(collection, name: str, mesh, *, parent=None, location=None):
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    if parent is not None:
        obj.parent = parent
    if location is not None:
        obj.location = location
    return obj


def _build_case(case_name: str, passport: str) -> dict:
    _clear_scene()
    collection = bpy.data.collections.new("G1_EXPORT")
    bpy.context.scene.collection.children.link(collection)

    if case_name == "one_object":
        objects = [_link_object(
            collection, "g1_one", _triangle_mesh("g1_one_mesh"),
            location=(1.25, -2.5, 3.75))]
    elif case_name == "multi_shared_hierarchy_long":
        shared = _triangle_mesh("g1_shared_mesh", custom_normals=True)
        parent = _link_object(
            collection, "g1_parent", shared, location=(2.0, 3.0, 4.0))
        child = _link_object(
            collection, "g1_child", shared, parent=parent,
            location=(0.5, -0.25, 1.75))
        independent = _link_object(
            collection, "g1_independent", _triangle_mesh("g1_independent_mesh"),
            location=(-3.0, 0.75, 2.25))
        objects = [parent, child, independent]
    elif case_name == "combined_lods":
        lod1_shared = _triangle_mesh("g1_lod1_mesh", custom_normals=True)
        objects = [
            _link_object(
                collection, "g1_lod0", _triangle_mesh("g1_lod0_mesh"),
                location=(0.0, 0.0, 0.0)),
            _link_object(
                collection, "g1_lod1_part_a", lod1_shared,
                location=(0.125, 0.0, 0.0)),
            _link_object(
                collection, "g1_lod1_part_b", lod1_shared,
                location=(-0.125, 0.0, 0.0)),
        ]
    else:
        raise ValueError(f"unknown G1 case: {case_name}")

    for obj in objects:
        obj[PASSPORT_PROPERTY] = passport  # Carrier B: every MESH Model.
        obj[LOD_LEVEL_PROPERTY] = (
            1 if case_name == "combined_lods" and "lod1" in obj.name else 0)

    sentinel = bpy.data.objects.new(SENTINEL_NAME, None)
    sentinel.empty_display_type = "PLAIN_AXES"
    sentinel[PASSPORT_PROPERTY] = passport  # Carrier A comparison.
    collection.objects.link(sentinel)

    bpy.context.view_layer.update()

    def corner_normals(mesh):
        return [
            [round(float(component), 6) for component in item.vector]
            for item in mesh.corner_normals
        ]

    return {
        "objects": objects,
        "sentinel": sentinel,
        "source": {
            obj.name: {
                "parent": obj.parent.name if obj.parent else None,
                "location": [float(v) for v in obj.location],
                "matrix_world_translation": [
                    float(v) for v in obj.matrix_world.translation],
                "custom_normals": bool(obj.data.has_custom_normals),
                "corner_normals": corner_normals(obj.data),
                "mesh_datablock": obj.data.name,
            }
            for obj in objects
        },
    }


def _select_for_export(objects) -> None:
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def _export_fbx(path: Path, objects) -> float:
    _select_for_export(objects)
    started = time.perf_counter()
    result = bpy.ops.export_scene.fbx(
        filepath=str(path),
        use_selection=True,
        object_types={"MESH", "EMPTY"},
        use_custom_props=True,
        add_leaf_bones=False,
        bake_anim=False,
        axis_forward="X",
        axis_up="Z",
    )
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    if result != {"FINISHED"}:
        raise RuntimeError(f"FBX export failed: {result}")
    if not path.read_bytes().startswith(b"Kaydara FBX Binary"):
        raise AssertionError(f"not a binary FBX: {path}")
    return elapsed_ms


def _import_fbx(path: Path, importer: str) -> tuple[list, float]:
    _clear_scene()
    started = time.perf_counter()
    if importer == "python_io_scene_fbx":
        result = bpy.ops.import_scene.fbx(
            filepath=str(path), use_custom_props=True)
    elif importer == "native_ufbx":
        result = bpy.ops.wm.fbx_import(
            filepath=str(path), use_custom_props=True)
    else:
        raise ValueError(importer)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    if result != {"FINISHED"}:
        raise RuntimeError(f"{importer} failed: {result}")
    return list(bpy.context.selected_objects), elapsed_ms


def _analyse_import(objects: list, passport: str) -> dict:
    meshes = sorted((obj for obj in objects if obj.type == "MESH"),
                    key=lambda item: item.name)
    empties = sorted((obj for obj in objects if obj.type == "EMPTY"),
                     key=lambda item: item.name)
    mesh_values = [obj.get(PASSPORT_PROPERTY) for obj in meshes]
    mesh_lod_levels = [obj.get(LOD_LEVEL_PROPERTY) for obj in meshes]
    sentinel_values = [obj.get(PASSPORT_PROPERTY) for obj in empties
                       if obj.name.startswith(SENTINEL_NAME)]

    def sha256(value):
        return (hashlib.sha256(value.encode("utf-8")).hexdigest()
                if isinstance(value, str) else None)
    mesh_reference_counts = {}
    for obj in meshes:
        mesh_reference_counts.setdefault(obj.data, 0)
        mesh_reference_counts[obj.data] += 1
    return {
        "mesh_count": len(meshes),
        "empty_count": len(empties),
        "mesh_model_passports_equal": bool(meshes) and all(
            value == passport for value in mesh_values),
        "carrier_b_copy_count": sum(value == passport for value in mesh_values),
        "carrier_b_copy_sha256": [sha256(value) for value in mesh_values],
        "mesh_lod_levels": {
            obj.name: obj.get(LOD_LEVEL_PROPERTY) for obj in meshes
        },
        "mesh_lod_levels_are_int": all(
            isinstance(value, int) and not isinstance(value, bool)
            for value in mesh_lod_levels),
        "carrier_a_survived": sentinel_values == [passport],
        "carrier_a_copy_count": sum(
            value == passport for value in sentinel_values),
        "carrier_a_copy_sha256": [sha256(value) for value in sentinel_values],
        "all_carrier_b_copies_byte_equal": (
            len(set(mesh_values)) == 1 and mesh_values[0] == passport
            if mesh_values else False),
        "mesh_names": [obj.name for obj in meshes],
        "parents": {
            obj.name: obj.parent.name if obj.parent else None for obj in meshes
        },
        "locations": {
            obj.name: [round(float(v), 6) for v in obj.location]
            for obj in meshes
        },
        "world_translations": {
            obj.name: [round(float(v), 6) for v in obj.matrix_world.translation]
            for obj in meshes
        },
        "custom_normals": {
            obj.name: bool(obj.data.has_custom_normals) for obj in meshes
        },
        "corner_normals": {
            obj.name: [
                [round(float(component), 6) for component in item.vector]
                for item in obj.data.corner_normals
            ]
            for obj in meshes
        },
        "unique_mesh_datablocks": len({obj.data.name for obj in meshes}),
        "mesh_datablock_reported_users": {
            mesh.name: mesh.users for mesh in mesh_reference_counts
        },
        "mesh_datablock_user_counts_consistent": all(
            mesh.users == references
            for mesh, references in mesh_reference_counts.items()
        ),
    }


def _corner_normal_max_abs_error(source: dict, imported: dict) -> float:
    errors = []
    for name, row in source.items():
        actual = imported.get(name, [])
        expected = row["corner_normals"]
        if len(actual) != len(expected):
            return float("inf")
        for actual_normal, expected_normal in zip(actual, expected):
            if len(actual_normal) != len(expected_normal):
                return float("inf")
            errors.extend(abs(a - b) for a, b in zip(
                actual_normal, expected_normal))
    return max(errors, default=0.0)


@contextmanager
def _cleanup_spike_scene_on_exit():
    # The spike is normally run with --factory-startup.  Tests also call it
    # in an isolated Blender process; do not pretend this restores user data.
    try:
        yield
    finally:
        _clear_scene()


def run_spike(outdir: str | Path) -> dict:
    outdir = Path(outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    importers = ("python_io_scene_fbx", "native_ufbx")
    case_specs = (
        ("one_object", passport_document(name="g1_one")),
        ("multi_shared_hierarchy_long", passport_document(
            name="g1_multi", long_probe_bytes=256 * 1024)),
        ("combined_lods", passport_document(
            name="g1_lod_asset", lod_levels=[0, 1])),
    )
    receipt = {
        "spike": "G1 Passport transport",
        "blender_version": bpy.app.version_string,
        "exporter": "bpy.ops.export_scene.fbx (io_scene_fbx)",
        "importers": list(importers),
        "passport_property": PASSPORT_PROPERTY,
        "carrier_b": "same canonical passport on every exported MESH Model",
        "carrier_a": "one exported Empty sentinel",
        "carrier_c": "rejected: requires post-write FBX mutation/parser",
        "ue_runtime": "BLOCKED: requires independent UE FBX SDK read receipt",
        "cases": {},
    }

    with _cleanup_spike_scene_on_exit():
        for case_name, document in case_specs:
            passport = canonical_passport(document)
            built = _build_case(case_name, passport)
            selected = [*built["objects"], built["sentinel"]]
            path = outdir / f"{case_name}.fbx"
            export_ms = _export_fbx(path, selected)
            case_receipt = {
                "fbx": path.name,
                "fbx_bytes": path.stat().st_size,
                "passport_utf8_bytes": len(passport.encode("utf-8")),
                "passport_sha256": hashlib.sha256(
                    passport.encode("utf-8")).hexdigest(),
                "passport_one_line": "\n" not in passport and "\r" not in passport,
                "unicode_nfc_utf8_present": "Гараж_é" in passport,
                "passport_is_unicode_nfc": (
                    unicodedata.normalize("NFC", passport) == passport),
                "passport_summary": {
                    key: document[key] for key in (
                        "schema", "schema_version", "resource_uid", "kind",
                        "name", "lod_levels", "lod_policy", "geometry_hash")
                },
                "source": built["source"],
                "export_ms": round(export_ms, 3),
                "imports": {},
            }
            for importer in importers:
                imported, import_ms = _import_fbx(path, importer)
                analysed = _analyse_import(imported, passport)
                source = case_receipt["source"]
                analysed["local_locations_match_source"] = all(
                    analysed["locations"].get(name) == [
                        round(float(value), 6) for value in row["location"]]
                    for name, row in source.items()
                )
                analysed["world_translations_match_source"] = all(
                    analysed["world_translations"].get(name) == [
                        round(float(value), 6)
                        for value in row["matrix_world_translation"]]
                    for name, row in source.items()
                )
                normal_error = _corner_normal_max_abs_error(
                    source, analysed["corner_normals"])
                analysed["corner_normal_max_abs_error"] = round(
                    normal_error, 8)
                analysed["corner_normals_within_5e_5"] = normal_error <= 5e-5
                analysed["import_ms"] = round(import_ms, 3)
                case_receipt["imports"][importer] = analysed
            receipt["cases"][case_name] = case_receipt

        # Re-export the combined LOD file to prove both Model properties survive
        # Python import -> FBX export -> both Blender importers.
        source_path = outdir / "combined_lods.fbx"
        imported, first_import_ms = _import_fbx(
            source_path, "python_io_scene_fbx")
        reexport_path = outdir / "combined_lods.reexport.fbx"
        reexport_ms = _export_fbx(reexport_path, imported)
        expected = canonical_passport(case_specs[2][1])
        receipt["reexport"] = {
            "source_importer": "python_io_scene_fbx",
            "source_import_ms": round(first_import_ms, 3),
            "export_ms": round(reexport_ms, 3),
            "fbx": reexport_path.name,
            "fbx_bytes": reexport_path.stat().st_size,
            "imports": {},
        }
        for importer in importers:
            objects, import_ms = _import_fbx(reexport_path, importer)
            analysed = _analyse_import(objects, expected)
            original_source = receipt["cases"][
                "combined_lods"]["source"]
            analysed["local_locations_match_source"] = all(
                analysed["locations"].get(name) == [
                    round(float(value), 6) for value in row["location"]]
                for name, row in original_source.items()
            )
            analysed["world_translations_match_source"] = all(
                analysed["world_translations"].get(name) == [
                    round(float(value), 6)
                    for value in row["matrix_world_translation"]]
                for name, row in original_source.items()
            )
            normal_error = _corner_normal_max_abs_error(
                original_source, analysed["corner_normals"])
            analysed["corner_normal_max_abs_error"] = round(normal_error, 8)
            analysed["corner_normals_within_5e_5"] = normal_error <= 5e-5
            analysed["import_ms"] = round(import_ms, 3)
            receipt["reexport"]["imports"][importer] = analysed

    result_path = outdir / "g1_results.json"
    result_path.write_text(
        json.dumps(receipt, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    receipt["result_path"] = str(result_path)
    return receipt


def _args(argv: list[str]) -> argparse.Namespace:
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    parser = argparse.ArgumentParser()
    parser.add_argument("--outdir", required=True)
    return parser.parse_args(argv)


def main() -> int:
    args = _args(sys.argv)
    receipt = run_spike(args.outdir)
    print(json.dumps(receipt, indent=2, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
