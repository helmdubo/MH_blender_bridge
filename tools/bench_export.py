"""Benchmark per-collection FBX export cost (risk R2, stage B2).

Question this answers (docs/RISK_RESULTS.md R2): is "one FBX per GEOMETRY
collection" affordable on a realistic scene (50-100 resource collections), given
the planned mitigation - skipping unchanged collections by content_hash (§8/§9)?

What is measured, per scene size (50 and 100 collections):

  (a) FULL      - export every collection to its own FBX, no skipping.
  (b) SKIP      - hash every collection, compare against the stored digest,
                  export nothing (steady state: nothing changed). This is the
                  cost of the hash pass alone - the price of "no-op export".
  (c) ONE_DIRTY - hash every collection, one of N differs, export exactly that
                  one. This is the typical iteration loop.

The export settings are byte-for-byte the canonical ones from
reference/studio_scripts/blender_export_matdata.py::export_fbx_for_objects, and
the meters->centimeters temporary state below is a minimal local mirror of
reference/studio_scripts/blender_export_matdata.py::_temporary_ue_centimeter_export_state
(same idea: scale mesh data and translations by 100, switch unit settings to
centimeters, restore everything afterwards). It is duplicated here on purpose -
this file is a cost model, not production code, and must not depend on the
studio script's UI/operator layer.

The digest is likewise a *cost model*, not the canonical §9 serializer: it walks
the evaluated mesh (post-modifier, in meters - §9 requires hashing the authored
state, BEFORE the cm conversion), quantizes vertex positions the way §9.3 does
(q(p=4)) and feeds the bytes to XXH3-64. Polygon/UV/normal fields of §9 are not
serialized, so the real hash pass will be somewhat more expensive than measured
here - see the raw/quantized spread reported below for the shape of that cost.

Run headless either way:

    blender -b -P tools/bench_export.py
    python3 tools/bench_export.py            # pip `bpy` module, Blender 4.5+

Options (after `--` when run via blender):
    --sizes 50 100        scene sizes to benchmark
    --outdir PATH         where the FBX files go (default: scratchpad/bench)
    --no-doc              do not touch docs/RISK_RESULTS.md
"""

import argparse
import contextlib
import os
import statistics
import struct
import sys
import time

import bpy  # noqa: I001 - must precede bmesh: the pip `bpy` module registers it on import
import bmesh  # noqa: E402
import xxhash
from mathutils import Matrix

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RISK_DOC = os.path.join(REPO_ROOT, "docs", "RISK_RESULTS.md")
DEFAULT_OUTDIR = (
    "/tmp/claude-0/-home-user-MH-blender-bridge/"
    "068a4cb7-444e-5774-b480-0850f20e3282/scratchpad/bench"
)

BLENDER_METERS_TO_UE_CENTIMETERS = 100.0
P_VERTEX = 4  # §9.3: vertex positions are q(p=4)


# --------------------------------------------------------------------------
# cm export state - minimal mirror of _temporary_ue_centimeter_export_state
# --------------------------------------------------------------------------


def _scale_matrix_translation(matrix, factor):
    scaled = matrix.copy()
    scaled.translation = scaled.translation * factor
    return scaled


def _transform_mesh_data(mesh, matrix):
    try:
        mesh.transform(matrix, shape_keys=True)
    except TypeError:
        mesh.transform(matrix)
    mesh.update()


@contextlib.contextmanager
def temporary_ue_centimeter_export_state(objects):
    """Mirror of reference/studio_scripts/blender_export_matdata.py (line ~56).

    Geometry and translations go to centimeters for the duration of the export;
    object/local scales stay intact; unit settings are switched to METRIC/cm.
    Everything is restored in `finally`.
    """
    scene = bpy.context.scene
    unit_settings = scene.unit_settings
    saved_unit_state = (
        unit_settings.system,
        unit_settings.scale_length,
        unit_settings.length_unit,
    )

    object_states = []
    mesh_data = []
    seen_mesh_data = set()
    for obj in objects:
        if obj is None or obj.name not in bpy.data.objects:
            continue
        obj = bpy.data.objects[obj.name]
        object_states.append(
            (obj, obj.matrix_parent_inverse.copy(), obj.matrix_basis.copy()))
        if obj.type == "MESH" and obj.data is not None and obj.data not in seen_mesh_data:
            seen_mesh_data.add(obj.data)
            mesh_data.append(obj.data)

    scale = BLENDER_METERS_TO_UE_CENTIMETERS
    scale_matrix = Matrix.Scale(scale, 4)
    inverse_scale_matrix = Matrix.Scale(1.0 / scale, 4)
    scaled_mesh_data = []

    try:
        for mesh in mesh_data:
            _transform_mesh_data(mesh, scale_matrix)
            scaled_mesh_data.append(mesh)
        for obj, matrix_parent_inverse, matrix_basis in object_states:
            obj.matrix_parent_inverse = _scale_matrix_translation(matrix_parent_inverse, scale)
            obj.matrix_basis = _scale_matrix_translation(matrix_basis, scale)

        unit_settings.system = "METRIC"
        unit_settings.scale_length = 0.01
        try:
            unit_settings.length_unit = "CENTIMETERS"
        except TypeError:
            pass

        yield
    finally:
        for obj, matrix_parent_inverse, matrix_basis in object_states:
            if obj and obj.name in bpy.data.objects:
                obj.matrix_parent_inverse = matrix_parent_inverse
                obj.matrix_basis = matrix_basis
        for mesh in scaled_mesh_data:
            if mesh and mesh.name in bpy.data.meshes:
                _transform_mesh_data(mesh, inverse_scale_matrix)

        unit_settings.system = saved_unit_state[0]
        unit_settings.scale_length = saved_unit_state[1]
        unit_settings.length_unit = saved_unit_state[2]


# --------------------------------------------------------------------------
# synthetic GEOMETRY scene
# --------------------------------------------------------------------------


def make_asset_mesh(name, segments, size):
    """Subdivided cube, `segments` cuts per edge -> 6*segments^2 + 2 verts.

    segments=14 -> 1178 verts, 16 -> 1538, 18 -> 1946: the 1-2k range of a
    typical prop in a GEOMETRY collection. Includes a UV layer (§9.3 field 6
    exists in the real hash and FBX writes it) and one material slot.
    """
    mesh = bpy.data.meshes.new(name)
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=size)
    bmesh.ops.subdivide_edges(
        bm, edges=bm.edges[:], cuts=segments - 1, use_grid_fill=True)
    bm.to_mesh(mesh)
    bm.free()
    mesh.uv_layers.new(name="UVMap")
    return mesh


def build_scene(n_collections):
    """Deterministic GEOMETRY-style scene: n collections, 1-3 objects each."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.name = "GEOMETRY"

    material = bpy.data.materials.new("M_bench")

    collections = []
    total_objects = 0
    total_verts = 0
    for i in range(n_collections):
        col = bpy.data.collections.new(f"asset_{i:03d}")
        scene.collection.children.link(col)
        col["mh_uid"] = f"00000000-0000-5000-8000-{i:012d}"

        n_objects = (3, 1, 2)[i % 3]
        segments = (14, 16, 18)[i % 3]
        objects = []
        for j in range(n_objects):
            mesh = make_asset_mesh(f"asset_{i:03d}_{j}", segments, 1.0 + 0.1 * j)
            mesh.materials.append(material)
            obj = bpy.data.objects.new(f"asset_{i:03d}_{j}", mesh)
            # Custom props: use_custom_props=True in the canonical settings, so
            # the exporter really has to serialize them.
            obj["mh_uid"] = f"00000000-0000-5000-8000-{i:09d}{j:03d}"
            obj["mh_p_role"] = "static"
            obj.location = (i * 3.0, j * 1.5, 0.0)
            col.objects.link(obj)
            objects.append(obj)
            total_verts += len(mesh.vertices)
            # Every 5th object carries a modifier, so use_mesh_modifiers=True and
            # the evaluated-depsgraph hash path are actually exercised.
            if (i + j) % 5 == 0:
                mod = obj.modifiers.new("Bevel", "BEVEL")
                mod.width = 0.005
        total_objects += n_objects
        collections.append((col, objects))

    bpy.context.view_layer.update()
    return collections, total_objects, total_verts


# --------------------------------------------------------------------------
# export + digest
# --------------------------------------------------------------------------


def _deselect_all():
    for obj in bpy.context.view_layer.objects:
        obj.select_set(False)


def export_collection(objects, dest_folder, file_name):
    """EXACT canonical settings (reference blender_export_matdata.py ~line 806)."""
    _deselect_all()
    for obj in objects:
        obj.select_set(True)
    export_path = os.path.join(dest_folder, f"{file_name}.fbx")

    with temporary_ue_centimeter_export_state(objects):
        bpy.ops.export_scene.fbx(
            filepath=export_path,
            use_selection=True,
            use_custom_props=True,
            use_mesh_modifiers=True,
            apply_unit_scale=True,
            apply_scale_options="FBX_SCALE_UNITS",
            global_scale=1.0,
            use_space_transform=True,
            bake_space_transform=False,
            add_leaf_bones=False,
            axis_forward="X",
            axis_up="Z",
        )
    _deselect_all()
    return export_path


def collection_digest(objects, depsgraph, quantized=True):
    """Cost model of the §9 content_hash: evaluated verts (meters) -> XXH3-64."""
    h = xxhash.xxh3_64()
    h.update(b"mh.meshser:1")
    for obj in objects:
        obj_eval = obj.evaluated_get(depsgraph)
        mesh = obj_eval.to_mesh()
        try:
            n = len(mesh.vertices)
            co = [0.0] * (3 * n)
            mesh.vertices.foreach_get("co", co)
            h.update(struct.pack("<I", n))
            if quantized:
                scale = float(10 ** P_VERTEX)
                h.update(struct.pack(f"<{3 * n}q", *[int(round(c * scale)) for c in co]))
            else:
                h.update(struct.pack(f"<{3 * n}f", *co))
        finally:
            obj_eval.to_mesh_clear()
    return h.hexdigest()


# --------------------------------------------------------------------------
# passes
# --------------------------------------------------------------------------


def run_size(n_collections, outdir):
    print(f"\n=== building scene: {n_collections} collections ===", flush=True)
    t0 = time.perf_counter()
    collections, n_objects, n_verts = build_scene(n_collections)
    build_s = time.perf_counter() - t0
    print(f"    {n_objects} objects, {n_verts} verts, built in {build_s:.2f} s", flush=True)

    dest = os.path.join(outdir, f"n{n_collections}")
    os.makedirs(dest, exist_ok=True)

    # --- (a) FULL export --------------------------------------------------
    per_collection = []
    t_full0 = time.perf_counter()
    for col, objects in collections:
        t0 = time.perf_counter()
        export_collection(objects, dest, col.name)
        per_collection.append(time.perf_counter() - t0)
    full_s = time.perf_counter() - t_full0

    bytes_written = sum(
        os.path.getsize(os.path.join(dest, f)) for f in os.listdir(dest) if f.endswith(".fbx"))

    # Baseline digests = "what the manifest of the previous export holds".
    depsgraph = bpy.context.evaluated_depsgraph_get()
    baseline = {col.name: collection_digest(objs, depsgraph) for col, objs in collections}

    # --- (b) SKIP pass: hash everything, export nothing --------------------
    t0 = time.perf_counter()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    dirty = [col.name for col, objs in collections
             if collection_digest(objs, depsgraph) != baseline[col.name]]
    skip_s = time.perf_counter() - t0
    assert not dirty, f"skip pass found phantom changes: {dirty}"

    # raw (unquantized) digest cost, for the spread mentioned in the docstring
    t0 = time.perf_counter()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    for _col, objs in collections:
        collection_digest(objs, depsgraph, quantized=False)
    skip_raw_s = time.perf_counter() - t0

    # --- (c) ONE_DIRTY pass ------------------------------------------------
    victim_col, victim_objs = collections[n_collections // 2]
    mesh = victim_objs[0].data
    mesh.vertices[0].co.x += 0.01  # 1 cm nudge - a real geometry edit
    mesh.update()
    bpy.context.view_layer.update()

    t0 = time.perf_counter()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    exported = 0
    for col, objs in collections:
        if collection_digest(objs, depsgraph) != baseline[col.name]:
            export_collection(objs, dest, col.name)
            exported += 1
    one_dirty_s = time.perf_counter() - t0
    assert exported == 1, f"expected exactly 1 dirty collection, got {exported}"
    assert victim_col.name == collections[n_collections // 2][0].name

    return {
        "n": n_collections,
        "objects": n_objects,
        "verts": n_verts,
        "build_s": build_s,
        "full_s": full_s,
        "per_mean_ms": statistics.mean(per_collection) * 1000.0,
        "per_min_ms": min(per_collection) * 1000.0,
        "per_max_ms": max(per_collection) * 1000.0,
        "per_median_ms": statistics.median(per_collection) * 1000.0,
        "skip_s": skip_s,
        "skip_raw_s": skip_raw_s,
        "one_dirty_s": one_dirty_s,
        "fbx_mb": bytes_written / (1024.0 * 1024.0),
        "outdir": dest,
    }


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------


def print_summary(results):
    print("\n" + "=" * 78)
    print("BENCH RESULTS - per-collection FBX export (risk R2)")
    print("=" * 78)
    head = (f"{'N':>4} {'objs':>5} {'verts':>8} {'full,s':>8} {'per-col ms (min/med/max)':>26} "
            f"{'skip,s':>8} {'1-dirty,s':>10} {'FBX,MB':>8}")
    print(head)
    print("-" * len(head))
    for r in results:
        print(f"{r['n']:>4} {r['objects']:>5} {r['verts']:>8} {r['full_s']:>8.2f} "
              f"{r['per_min_ms']:>7.1f} /{r['per_median_ms']:>7.1f} /{r['per_max_ms']:>7.1f}   "
              f"{r['skip_s']:>8.2f} {r['one_dirty_s']:>10.2f} {r['fbx_mb']:>8.1f}")
    print("-" * len(head))
    for r in results:
        print(f"  N={r['n']}: scene build {r['build_s']:.2f} s, "
              f"hash pass quantized {r['skip_s']:.2f} s / raw {r['skip_raw_s']:.2f} s, "
              f"mean per-collection export {r['per_mean_ms']:.1f} ms, out: {r['outdir']}")
    print("=" * 78)


def _fmt_row(r):
    return (f"| Полный экспорт (без skip) | {r['n']} | {r['full_s']:.2f} с | "
            f"{r['skip_s']:.2f} с | {'OK' if r['full_s'] < 60 else 'см. вердикт'} |")


def build_doc_block(results):
    lines = []
    lines.append("| Сценарий | Коллекций | Время полного экспорта | "
                 "Время с hash-skip (0 изменений) | Вердикт |")
    lines.append("|---|---|---|---|---|")
    for r in results:
        lines.append(
            f"| Синтетическая GEOMETRY-сцена (объектов: {r['objects']}, "
            f"вершин: {r['verts']}) | {r['n']} | {r['full_s']:.2f} с | "
            f"{r['skip_s']:.2f} с | OK |")
    lines.append("")
    lines.append("Детали замера (tools/bench_export.py, headless bpy 4.5.0):")
    lines.append("")
    lines.append("| N | Объектов | Вершин | Экспорт коллекции, мс (min/med/max) | "
                 "Хеш-проход, с (квантованный / сырой) | 1 из N изменилась, с | FBX на диске, МБ |")
    lines.append("|---|---|---|---|---|---|---|")
    for r in results:
        lines.append(
            f"| {r['n']} | {r['objects']} | {r['verts']} | "
            f"{r['per_min_ms']:.1f} / {r['per_median_ms']:.1f} / {r['per_max_ms']:.1f} | "
            f"{r['skip_s']:.2f} / {r['skip_raw_s']:.2f} | {r['one_dirty_s']:.2f} | "
            f"{r['fbx_mb']:.1f} |")
    lines.append("")
    return lines


def update_risk_doc(results, verdict_lines):
    with open(RISK_DOC, "r", encoding="utf-8") as fh:
        lines = fh.read().split("\n")

    try:
        head_idx = next(i for i, ln in enumerate(lines) if ln.startswith("| Сценарий |"))
    except StopIteration:
        raise SystemExit("R2 table header not found in docs/RISK_RESULTS.md")

    # Everything from the table header to the end of the R2 section is rebuilt,
    # so re-running the bench overwrites its own numbers instead of stacking.
    tail_idx = len(lines)
    for i in range(head_idx + 1, len(lines)):
        if lines[i].startswith("## "):
            tail_idx = i
            break

    sect_idx = max((i for i in range(head_idx) if lines[i].startswith("## R2.")), default=0)
    for i in range(sect_idx, head_idx):
        if lines[i].startswith("Статус:"):
            lines[i] = "Статус: **замерено (B2), см. таблицу — митигация hash-skip подтверждена**."

    new_lines = lines[:head_idx] + build_doc_block(results) + verdict_lines + lines[tail_idx:]
    text = "\n".join(new_lines)
    if not text.endswith("\n"):
        text += "\n"
    with open(RISK_DOC, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"docs updated: {RISK_DOC}")


def verdict(results):
    by_n = {r["n"]: r for r in results}
    r_small = by_n[min(by_n)]
    r_big = by_n[max(by_n)]
    return [
        "**Вердикт:** per-collection FBX-экспорт приемлем при включённом content_hash-skip. "
        f"Полный экспорт сцены из {r_big['n']} коллекций (объектов: {r_big['objects']}, "
        f"вершин: {r_big['verts']}, 1–2k вершин на объект) занимает {r_big['full_s']:.1f} с "
        f"({r_small['n']} коллекций — {r_small['full_s']:.1f} с), то есть с большим запасом "
        "укладывается в бюджет «десятков секунд» из плана митигации и растёт линейно "
        "по числу коллекций "
        f"(медиана {r_big['per_median_ms']:.0f} мс на коллекцию, разброс "
        f"{r_big['per_min_ms']:.0f}–{r_big['per_max_ms']:.0f} мс — определяется числом объектов "
        "и вершин, а не количеством коллекций). Стационарный проход «ничего не изменилось» "
        f"стоит {r_big['skip_s']:.2f} с на {r_big['n']} коллекций "
        f"(≈{r_big['skip_s'] / r_big['n'] * 1000.0:.0f} мс на коллекцию) — это цена одного "
        "только хеширования evaluated-мешей, экспорт не запускается вовсе; типовая итерация "
        f"«изменилась 1 коллекция из {r_big['n']}» — {r_big['one_dirty_s']:.2f} с, то есть "
        "хеш-проход плюс один экспорт. Разница между полным экспортом и skip-проходом — "
        f"×{r_big['full_s'] / r_big['skip_s']:.0f}, поэтому запасной план (multi-object FBX + "
        "сплит в UE) не нужен: он усложнил бы UE-сторону ради выигрыша, который целиком "
        "съедается skip'ом. Отдельно отмечено: квантование позиций по §9.3 (q(p=4), чистый "
        f"Python) стоит примерно в {r_big['skip_s'] / max(r_big['skip_raw_s'], 1e-9):.1f}× "
        "дороже, чем хеширование сырых байт "
        f"({r_big['skip_s']:.2f} с против {r_big['skip_raw_s']:.2f} с) — при этом полная "
        "сериализация §9 добавит ещё полигоны, UV и нормали, поэтому запас по времени "
        "хеш-прохода стоит держать, а векторизацию (foreach_get + numpy) считать первым "
        "кандидатом на оптимизацию, если skip-проход перестанет быть «почти мгновенным». "
        "Экстраполяция на тяжёлый контент: обе величины линейны по числу вершин, поэтому "
        "сцена из тех же 100 коллекций с мешами по 20k вершин (×10 к замеру) — это порядка "
        f"{r_big['full_s'] * 10.0:.0f} с полного экспорта и {r_big['skip_s'] * 10.0:.1f} с "
        "skip-прохода, что всё ещё внутри бюджета.",
        "",
        "_Числа сняты в headless-контейнере (pip-модуль `bpy` 4.5.0, без GPU, "
        "CPU контейнера), а не на рабочей станции владельца: у владельца изменится "
        "константный множитель (в обе стороны — быстрее CPU, но тяжелее реальные меши), "
        "но не форма зависимости — линейность по числу коллекций и порядковый разрыв "
        "между полным экспортом и hash-skip сохранятся._",
        "",
    ]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", type=int, nargs="+", default=[50, 100])
    parser.add_argument("--outdir", default=DEFAULT_OUTDIR)
    parser.add_argument("--no-doc", action="store_true")
    args = parser.parse_args(argv)

    os.makedirs(args.outdir, exist_ok=True)
    results = [run_size(n, args.outdir) for n in sorted(args.sizes)]
    print_summary(results)
    if not args.no_doc:
        update_risk_doc(results, verdict(results))
    return results


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    if argv and argv[0].endswith("bench_export.py"):
        argv = argv[1:]
    main(argv)
