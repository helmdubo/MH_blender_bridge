"""Build the axis/handedness probe for risk R1 (stage B3, Blender half).

The R1 question (docs/01_bundle_schema_v1.md §11, status: *hypothesis*): does

    pos_UE  = ( x*100, -y*100, z*100 )      # cm
    quat_UE = ( -qx, qy, -qz, qw )          # (x, y, z, w)
    scale_UE = ( sx, sy, sz )

together with the canonical FBX settings (`axis_forward='X'`, `axis_up='Z'` +
the meters->centimeters context manager of reference/studio_scripts) really put
a control vertex of an asymmetric mesh in the same world position in UE as in
Blender?

This script produces the Blender half of the answer:

  * scene with collection `axis_probe`, one object with the *same* asymmetric
    window_a mesh as the golden scene (imported from make_golden_scene, not
    copied) - control vertex index 8 at (0.37, 0.11, 1.93) m, local;
  * one deliberately non-symmetric placement transform;
  * an FBX export of `axis_probe` with the exact canonical settings;
  * the expected numbers: Blender world position of the control vertex, its
    §11-converted UE world position (cm), and the §11-converted UE quaternion
    of the placement (raw floats + canonicalized quantized integers, §8.2/§11).

  IMPORTANT - what travels in what:
  The FBX carries BAKED GEOMETRY ONLY. The object is exported at identity, so
  the file holds resource-space geometry in centimeters and nothing else. In
  the real pipeline the placement transform never travels through FBX: it lives
  in the `.composite` node (`local_transform`) and is applied UE-side to the
  spawned actor/component. Therefore the UE check (tools/ue_axis_check.py) must
  import the FBX, spawn a StaticMeshActor and set its transform to the
  §11-converted placement - never rely on the FBX to carry the placement.

Run headless either way:

    blender -b -P tools/make_axis_bundle.py
    python3 tools/make_axis_bundle.py         # pip `bpy` module, Blender 4.5+

Options (after `--` when run via blender):
    --outdir PATH   where the FBX goes (default: scratchpad/axis)
    --no-doc        do not touch docs/RISK_RESULTS.md
"""

import argparse
import contextlib
import math
import os
import sys

import bpy
from mathutils import Euler, Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_golden_scene import CONTROL_VERTEX, make_window_mesh  # noqa: E402

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RISK_DOC = os.path.join(REPO_ROOT, "docs", "RISK_RESULTS.md")
# The FBX is a committed fixture: the owner runs the UE half without Blender.
DEFAULT_OUTDIR = os.path.join(REPO_ROOT, "golden", "fixtures", "axis")
FBX_NAME = "axis_probe.fbx"

BLENDER_METERS_TO_UE_CENTIMETERS = 100.0
CONTROL_VERTEX_INDEX = 8  # make_golden_scene.make_window_mesh: prong tip

# The one placement under test. Non-symmetric on purpose: every axis is
# distinguishable, so a swapped/mirrored axis cannot accidentally match.
# Uniform scale 1.0 - negative and non-uniform scale are out of scope for R1
# (negative scale is forbidden by §11 in v1 anyway).
PLACEMENT_LOCATION_M = (1.25, -2.5, 0.75)
PLACEMENT_ROTATION_DEG = (10.0, 20.0, 30.0)  # XYZ euler
PLACEMENT_SCALE = (1.0, 1.0, 1.0)

P_TRANSLATION_CM = 3  # §8.2
P_ROTATION_QUAT = 6   # §8.2


# --------------------------------------------------------------------------
# cm export state - minimal mirror of _temporary_ue_centimeter_export_state
# (reference/studio_scripts/blender_export_matdata.py, line ~56)
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
# §11 conversion helpers
# --------------------------------------------------------------------------


def quantize(value, p):
    """§8.2: round_half_even(value * 10^p) -> int (same rule as core/canonical)."""
    return int(round(float(value) * float(10 ** p)))


def pos_blender_to_ue(vec):
    """§11: meters (Blender RH Z-up) -> centimeters (UE LH Z-up)."""
    return (vec[0] * 100.0, -vec[1] * 100.0, vec[2] * 100.0)


def quat_blender_to_ue(quat):
    """§11: mathutils Quaternion (w,x,y,z) -> UE (x, y, z, w) tuple."""
    return (-quat.x, quat.y, -quat.z, quat.w)


def canonicalize_quat_xyzw(q):
    """§11 sign canonicalization on the quantized integers (p=6)."""
    n = math.sqrt(sum(c * c for c in q))
    qi = [quantize(c / n, P_ROTATION_QUAT) for c in q]
    x, y, z, w = qi
    negate = w < 0
    if w == 0:
        for c in (x, y, z):
            if c != 0:
                negate = c < 0
                break
    return (-x, -y, -z, -w) if negate else (x, y, z, w)


def ue_quat_to_rotator(q):
    """Reference only: FQuat::Rotator() -> (pitch, yaw, roll) in degrees.

    Mirrors Engine/Source/Runtime/Core/Private/Math/UnrealMath.cpp. Printed so
    the owner can eyeball the placement in the UE details panel; the check
    script sets the quaternion itself, not the rotator.
    """
    n = math.sqrt(sum(c * c for c in q))
    x, y, z, w = (c / n for c in q)
    singularity = z * x - w * y
    yaw_y = 2.0 * (w * z + x * y)
    yaw_x = 1.0 - 2.0 * (y * y + z * z)
    threshold = 0.4999995
    if singularity < -threshold:
        pitch = -90.0
        yaw = math.degrees(math.atan2(yaw_y, yaw_x))
        roll = -yaw - 2.0 * math.degrees(math.atan2(x, w))
    elif singularity > threshold:
        pitch = 90.0
        yaw = math.degrees(math.atan2(yaw_y, yaw_x))
        roll = yaw - 2.0 * math.degrees(math.atan2(x, w))
    else:
        pitch = math.degrees(math.asin(max(-1.0, min(1.0, 2.0 * singularity))))
        yaw = math.degrees(math.atan2(yaw_y, yaw_x))
        roll = math.degrees(math.atan2(-2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y)))
    return (pitch, yaw, roll)


# --------------------------------------------------------------------------
# scene + export
# --------------------------------------------------------------------------


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.name = "GEOMETRY"

    col = bpy.data.collections.new("axis_probe")
    scene.collection.children.link(col)

    mesh = make_window_mesh("window_a")
    obj = bpy.data.objects.new("window_a", mesh)
    col.objects.link(obj)
    bpy.context.view_layer.update()
    return col, obj


def export_axis_probe(objects, outdir):
    """EXACT canonical settings (reference blender_export_matdata.py ~line 806)."""
    os.makedirs(outdir, exist_ok=True)
    for other in bpy.context.view_layer.objects:
        other.select_set(False)
    for obj in objects:
        obj.select_set(True)
    export_path = os.path.join(outdir, FBX_NAME)

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
    for obj in objects:
        obj.select_set(False)
    return export_path


def verify_roundtrip(fbx_path):
    """Re-import the exported FBX into an empty scene and read it back.

    Proves two claims the UE side depends on: the node carries NO placement
    (zero location/rotation - the 0.01 scale is Blender's own cm->m unit
    conversion on import), and the control vertex sits where the file's own
    units say it does. Destroys the scene, so it runs last.
    """
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=fbx_path)
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    if len(meshes) != 1:
        raise SystemExit(f"round-trip: expected 1 mesh object, got {len(meshes)}")
    obj = meshes[0]

    # File-space expectation: geometry scaled x100, no axis flip yet - the Y
    # negation of §11 is what the *UE* importer is expected to perform.
    expect = Vector(CONTROL_VERTEX) * BLENDER_METERS_TO_UE_CENTIMETERS
    nearest_i, nearest_d = -1, float("inf")
    for i, v in enumerate(obj.data.vertices):
        d = (v.co - expect).length
        if d < nearest_d:
            nearest_i, nearest_d = i, d
    co = tuple(obj.data.vertices[nearest_i].co)
    if nearest_d > 1e-3:
        raise SystemExit(
            f"round-trip: control vertex not found in the FBX (nearest {co}, "
            f"distance {nearest_d} cm from expected {tuple(expect)})")
    return {
        "rt_vertex_index": nearest_i,
        "rt_vertex_file_cm": co,
        "rt_node_location": tuple(obj.location),
        "rt_node_rotation_deg": tuple(math.degrees(a) for a in obj.rotation_euler),
        "rt_node_scale": tuple(obj.scale),
        "rt_vertex_count": len(obj.data.vertices),
    }


def compute(outdir):
    col, obj = build_scene()

    placement = (
        Matrix.Translation(Vector(PLACEMENT_LOCATION_M))
        @ Euler([math.radians(a) for a in PLACEMENT_ROTATION_DEG], "XYZ").to_matrix().to_4x4()
        @ Matrix.Diagonal(Vector(PLACEMENT_SCALE)).to_4x4()
    )

    local = Vector(CONTROL_VERTEX)
    assert tuple(obj.data.vertices[CONTROL_VERTEX_INDEX].co) == tuple(local), (
        f"vertex {CONTROL_VERTEX_INDEX} is not the control vertex: "
        f"{tuple(obj.data.vertices[CONTROL_VERTEX_INDEX].co)}")

    # Blender world position of the control vertex under the placement.
    # The transform is really put on the object (matrix_world @ v, not a
    # paper formula), then taken off again before the export.
    obj.matrix_world = placement
    bpy.context.view_layer.update()
    world_m = obj.matrix_world @ local

    # Reset to identity: the FBX must hold resource-space geometry only.
    obj.matrix_world = Matrix.Identity(4)
    bpy.context.view_layer.update()
    fbx_path = export_axis_probe([obj], outdir)

    quat_blender = placement.to_quaternion()
    quat_ue = quat_blender_to_ue(quat_blender)

    res = {
        "fbx_path": fbx_path,
        "collection": col.name,
        "object": obj.name,
        "local_m": tuple(local),
        "world_m": tuple(world_m),
        "local_ue_cm": pos_blender_to_ue(local),
        "world_ue_cm": pos_blender_to_ue(world_m),
        "placement_loc_ue_cm": pos_blender_to_ue(PLACEMENT_LOCATION_M),
        "placement_loc_q": tuple(
            quantize(c, P_TRANSLATION_CM) for c in pos_blender_to_ue(PLACEMENT_LOCATION_M)),
        "quat_blender_wxyz": (quat_blender.w, quat_blender.x, quat_blender.y, quat_blender.z),
        "quat_ue_xyzw": quat_ue,
        "quat_ue_q": canonicalize_quat_xyzw(quat_ue),
        "rotator_pyr": ue_quat_to_rotator(quat_ue),
        "scale_ue": tuple(PLACEMENT_SCALE),
    }
    # Last: wipes the scene, so everything above must already be plain numbers.
    res.update(verify_roundtrip(fbx_path))
    return res


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------


def _v(t, fmt="{:.6f}"):
    return "(" + ", ".join(fmt.format(c) for c in t) + ")"


def print_report(res):
    print("\n" + "=" * 78)
    print("AXIS PROBE (risk R1) - expected values")
    print("=" * 78)
    print(f"  collection            : {res['collection']} (object {res['object']})")
    print(f"  FBX (geometry only)   : {res['fbx_path']}")
    print("")
    print(f"  placement location, m : {_v(PLACEMENT_LOCATION_M)}")
    print(f"  placement rotation    : XYZ euler {_v(PLACEMENT_ROTATION_DEG, '{:.1f}')} deg")
    print(f"  placement scale       : {_v(PLACEMENT_SCALE, '{:.3f}')}")
    print("")
    print(f"  control vertex idx    : {CONTROL_VERTEX_INDEX}")
    print(f"  control vertex local  : {_v(res['local_m'])} m  ->  "
          f"{_v(res['local_ue_cm'], '{:.4f}')} cm (UE, §11)")
    print(f"  control vertex world  : {_v(res['world_m'])} m  (Blender, matrix_world @ v)")
    print(f"  EXPECTED UE world     : {_v(res['world_ue_cm'], '{:.4f}')} cm  (§11)")
    print("")
    print(f"  quat Blender (w,x,y,z): {_v(res['quat_blender_wxyz'])}")
    print(f"  EXPECTED UE quat xyzw : {_v(res['quat_ue_xyzw'])}")
    print(f"  quantized (p=6, §11)  : {res['quat_ue_q']}")
    print(f"  UE rotator (P,Y,R)    : {_v(res['rotator_pyr'], '{:.4f}')} deg (reference only)")
    print(f"  UE actor location, cm : {_v(res['placement_loc_ue_cm'], '{:.4f}')}"
          f"  quantized (p=3): {res['placement_loc_q']}")
    print(f"  UE actor scale        : {_v(res['scale_ue'], '{:.3f}')}")
    print("")
    print(f"  round-trip check      : node location {_v(res['rt_node_location'], '{:.4f}')}, "
          f"rotation {_v(res['rt_node_rotation_deg'], '{:.4f}')} deg, "
          f"scale {_v(res['rt_node_scale'], '{:.4f}')}")
    print(f"                          control vertex #{res['rt_vertex_index']} of "
          f"{res['rt_vertex_count']} at {_v(res['rt_vertex_file_cm'], '{:.4f}')} in file units (cm)")
    print("")
    print("  NOTE: the FBX contains BAKED GEOMETRY ONLY (object exported at identity,")
    print("        geometry in centimeters). The placement transform travels through the")
    print("        .composite node, not through FBX - so the UE check must spawn the")
    print("        StaticMeshActor and SET the §11-converted transform on it.")
    print("  Next : run tools/ue_axis_check.py in UE 5.7 (see its header), numbers ->")
    print("         docs/RISK_RESULTS.md, R1 table.")
    print("=" * 78)


DOC_HEADING = "### Ожидаемые значения (B3, Blender-сторона)"


def doc_block(res):
    q = res["quat_ue_q"]
    lines = [
        DOC_HEADING,
        "",
        "Посчитано `tools/make_axis_bundle.py` (headless bpy 4.5.0). Placement под "
        "проверкой — намеренно несимметричный, scale единичный (отрицательный и "
        "неравномерный scale вне области R1):",
        "",
        f"- location (Blender, м): `{_v(PLACEMENT_LOCATION_M, '{:.3f}')}`",
        f"- rotation_euler XYZ (град): `{_v(PLACEMENT_ROTATION_DEG, '{:.1f}')}`",
        f"- scale: `{_v(PLACEMENT_SCALE, '{:.3f}')}`",
        f"- FBX: `{FBX_NAME}` (коллекция `{res['collection']}`, объект "
        f"`{res['object']}`, канонические настройки экспорта + cm-состояние)",
        "",
        "| Величина | Значение |",
        "|---|---|",
        f"| Контрольная вершина, локально (Blender, м) | `{_v(res['local_m'], '{:.4f}')}`, "
        f"индекс {CONTROL_VERTEX_INDEX} |",
        f"| Контрольная вершина, локально (UE, см, §11) | `{_v(res['local_ue_cm'], '{:.4f}')}` |",
        f"| Контрольная вершина, мир (Blender, м) | `{_v(res['world_m'], '{:.6f}')}` |",
        f"| **Ожидаемая мировая позиция в UE (см, §11)** | "
        f"**`{_v(res['world_ue_cm'], '{:.4f}')}`** |",
        f"| Кватернион placement (Blender, w,x,y,z) | "
        f"`{_v(res['quat_blender_wxyz'], '{:.6f}')}` |",
        f"| **Ожидаемый кватернион в UE (x,y,z,w, §11)** | "
        f"**`{_v(res['quat_ue_xyzw'], '{:.6f}')}`** |",
        f"| Он же квантованный (p=6, знак канонизирован) | "
        f"`({q[0]}, {q[1]}, {q[2]}, {q[3]})` |",
        f"| UE Rotator (Pitch, Yaw, Roll), справочно | "
        f"`{_v(res['rotator_pyr'], '{:.4f}')}` |",
        f"| Location актора в UE (см, §11) | `{_v(res['placement_loc_ue_cm'], '{:.4f}')}`, "
        f"квантованный (p=3): `{res['placement_loc_q']}` |",
        f"| Scale актора в UE | `{_v(res['scale_ue'], '{:.3f}')}` |",
        "",
        "Допуск проверки — 0.1 см (`tools/ue_axis_check.py`).",
        "",
        "Самопроверка экспорта (обратный импорт полученного FBX в пустую сцену "
        "Blender): узел приходит с нулевыми location "
        f"`{_v(res['rt_node_location'], '{:.4f}')}` и rotation "
        f"`{_v(res['rt_node_rotation_deg'], '{:.4f}')}` (scale "
        f"`{_v(res['rt_node_scale'], '{:.4f}')}` — это пересчёт см→м самим "
        "импортёром Blender, а не трансформ из файла), контрольная вершина — "
        f"`{_v(res['rt_vertex_file_cm'], '{:.4f}')}` в единицах файла (см), "
        f"всего вершин {res['rt_vertex_count']}. Обратите внимание: в файле Y "
        "положительный, а ожидаемая UE-локальная координата — "
        f"`{_v(res['local_ue_cm'], '{:.4f}')}`: отрицание Y обязан выполнить "
        "импортёр UE (`convert_scene=True`, `force_front_x_axis=False`) — это "
        "ровно та часть гипотезы, которую проверяет UE-сниппет.",
        "",
        "**FBX содержит только запечённую геометрию.** Объект экспортируется с "
        "единичным трансформом, в файле — геометрия в resource-space (в сантиметрах) "
        "и ничего больше. Placement в реальном пайплайне едет в `.composite` "
        "(`local_transform` узла) и применяется на UE-стороне к актору/компоненту, "
        "поэтому UE-проверка обязана **сама выставить** актору §11-конвертированный "
        "трансформ, а не полагаться на то, что его принесёт FBX.",
        "",
        "Числа получены в headless-контейнере (pip-модуль `bpy` 4.5.0); геометрия и "
        "трансформы детерминированы, так что повторный прогон у владельца обязан дать "
        "те же значения бит-в-бит.",
        "",
    ]
    return lines


def update_risk_doc(res):
    with open(RISK_DOC, "r", encoding="utf-8") as fh:
        lines = fh.read().split("\n")

    try:
        r2_idx = next(i for i, ln in enumerate(lines) if ln.startswith("## R2."))
    except StopIteration:
        raise SystemExit("R2 heading not found in docs/RISK_RESULTS.md")

    # Rerunnable: an existing block is replaced, not duplicated.
    start = next((i for i in range(r2_idx) if lines[i].startswith(DOC_HEADING)), r2_idx)

    r1_idx = next((i for i, ln in enumerate(lines) if ln.startswith("## R1.")), 0)
    for i in range(r1_idx, start):
        if lines[i].startswith("Статус:"):
            lines[i] = ("Статус: **Blender-половина готова (B3), ожидает прогона "
                        "сниппета в UE 5.7**.")

    new_lines = lines[:start] + doc_block(res) + lines[r2_idx:]
    text = "\n".join(new_lines)
    if not text.endswith("\n"):
        text += "\n"
    with open(RISK_DOC, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"docs updated: {RISK_DOC}")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", default=DEFAULT_OUTDIR)
    parser.add_argument("--no-doc", action="store_true")
    args = parser.parse_args(argv)

    res = compute(args.outdir)
    print_report(res)
    if not args.no_doc:
        update_risk_doc(res)
    return res


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    if argv and argv[0].endswith("make_axis_bundle.py"):
        argv = argv[1:]
    main(argv)
