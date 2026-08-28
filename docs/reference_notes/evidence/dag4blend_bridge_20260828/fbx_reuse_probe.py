"""Diagnostic only: real immutable MH FBX staging, no source publication."""
import array
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import traceback

import bpy

OUT = Path(r"E:\MimirComposite_V5S6_1_DirectReuseProbe_20260828")
SNAPSHOT = OUT / "snapshot/addon"
sys.path.insert(0, str(SNAPSHOT))
from mh4blend.scene.export_fbx import prepare_fbx_collection, stage_prepared_fbx
from mh4blend.scene.import_fbx import parse_mesh_fbx
from io_scene_fbx import parse_fbx, export_fbx_bin, fbx_utils


def plain(value):
    if isinstance(value, bytes):
        return {"bytes_hex": value.hex()}
    if isinstance(value, array.array):
        return {"array_type": value.typecode, "values": list(value)}
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, (list, tuple)):
        return [plain(v) for v in value]
    return repr(value)


def flatten(elem, prefix=""):
    output = {}
    counts = {}
    for child in elem.elems:
        name = child.id.decode("ascii", errors="backslashreplace")
        index = counts.get(name, 0)
        counts[name] = index + 1
        path = f"{prefix}/{name}[{index}]"
        output[path] = {"props": plain(child.props), "types": list(child.props_type)}
        output.update(flatten(child, path))
    return output


def differences(left, right):
    return [{"path": key, "left": left.get(key), "right": right.get(key)}
            for key in sorted(set(left) | set(right)) if left.get(key) != right.get(key)]


def main():
    args = sys.argv[sys.argv.index("--")+1:]
    run = args[0]
    run_root = OUT / run
    run_root.mkdir()
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    collection = bpy.data.collections.new("reuse_probe")
    bpy.context.scene.collection.children.link(collection)
    collection["type"] = "rendinst"
    collection["name"] = "reuse_probe"
    mesh = bpy.data.meshes.new("reuse_probe_geo")
    mesh.from_pydata([(0,0,0), (1,0,0), (0,1,0)], [], [(0,1,2)])
    mesh.update()
    obj = bpy.data.objects.new("reuse_probe_mesh", mesh)
    collection.objects.link(obj)
    prepared = prepare_fbx_collection(collection, OUT / "source", source_root=OUT / "source")
    baseline_fingerprint = prepared.authority_fingerprint
    staged_rows = []
    trees = []
    plans = []
    for label in ("a", "b", "vertex_edit"):
        if label == "vertex_edit":
            mesh.vertices[1].co.x = 2
            mesh.update()
        stage = run_root / label
        stage.mkdir()
        refreshed = prepare_fbx_collection(collection, OUT / "source", source_root=OUT / "source")
        result = stage_prepared_fbx(refreshed, stage / "reuse_probe.mesh.fbx")
        root, version = parse_fbx.parse(str(result.filepath))
        tree = flatten(root)
        plan = asdict(parse_mesh_fbx(result.filepath))
        staged_rows.append({"label":label, "path":str(result.filepath), "bytes":len(result.payload),
                            "sha256":hashlib.sha256(result.payload).hexdigest(), "fbx_version":version,
                            "authority_fingerprint_equals_baseline": refreshed.authority_fingerprint == baseline_fingerprint})
        trees.append(tree)
        plans.append(plan)
        (run_root / f"{label}_tree.json").write_text(json.dumps(tree, indent=2)+"\n", encoding="utf-8", newline="\n")
        time.sleep(0.03)
    report = {"run":run, "pid":os.getpid(), "blender_version":bpy.app.version_string,
              "binary":bpy.app.binary_path, "source_snapshot_commit":"5f566c7b16e36fa68e1e5ef1391675d1c5febf2d",
              "mh_writer_path":str(SNAPSHOT / "mh4blend/scene/export_fbx.py"),
              "mh_writer_sha256":hashlib.sha256((SNAPSHOT / "mh4blend/scene/export_fbx.py").read_bytes()).hexdigest(),
              "fbx_exporter_path":export_fbx_bin.__file__, "fbx_uuid_path":fbx_utils.__file__,
              "stages":staged_rows,
              "unchanged_raw_equal":staged_rows[0]["sha256"] == staged_rows[1]["sha256"],
              "unchanged_tree_differences":differences(trees[0],trees[1]),
              "vertex_edit_tree_differences":differences(trees[1],trees[2]),
              "classification_plan_unchanged_equal":plans[0] == plans[1],
              "classification_plan_vertex_edit_equal":plans[1] == plans[2],
              "classification_plan":plans[0],
              "scene_collection_properties":{key:collection[key] for key in collection.keys()},
              "source_files_created":list((OUT / "source").iterdir())}
    assert not report["source_files_created"]
    (OUT / "reports" / f"{run}.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8", newline="\n")
    print(json.dumps({key:report[key] for key in ("run", "stages", "unchanged_raw_equal", "classification_plan_vertex_edit_equal", "scene_collection_properties")}, indent=2), flush=True)


try:
    main()
except BaseException:
    failure = traceback.format_exc()
    (OUT / "reports/probe_failure.txt").write_text(failure, encoding="utf-8")
    print(failure, flush=True)
    raise
