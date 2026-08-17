# -*- coding: utf-8 -*-
"""UE-сторона go/no-go проверки R1 (axis/handedness, §11 схемы). ЗАПУСКАЕТ ВЛАДЕЛЕЦ ВРУЧНУЮ.

ЧТО ПРОВЕРЯЕМ
-------------
Гипотеза §11 (docs/01_bundle_schema_v1.md): при каноническом FBX-экспорте
(`axis_forward='X'`, `axis_up='Z'` + cm-контекст-менеджер reference/studio_scripts)
перевод трансформа Blender → UE выражается формулами

    pos_UE  = ( x*100, -y*100, z*100 )      # см
    quat_UE = ( -qx, qy, -qz, qw )          # (x, y, z, w)
    scale_UE = ( sx, sy, sz )

Арбитр — мировая позиция контрольной вершины асимметричного меша `window_a`
(вершина-«шип», локально `(0.37, 0.11, 1.93)` м) при несимметричном placement:
Blender и UE обязаны сойтись с точностью 0.1 см.

ВАЖНО ПРО FBX: файл содержит ТОЛЬКО запечённую геометрию (объект экспортирован с
единичным трансформом, геометрия в сантиметрах). Placement в реальном пайплайне
едет в `.composite` и применяется на UE-стороне, поэтому скрипт САМ выставляет
актору §11-конвертированный трансформ и не полагается на то, что его принесёт FBX.

ПРОЦЕДУРА ДЛЯ ВЛАДЕЛЬЦА
-----------------------
1. Получить FBX: он уже лежит в репозитории —

       golden/fixtures/axis/axis_probe.fbx

   Blender для проверки НЕ нужен. (Перегенерация при желании:
   `blender -b -P tools/make_axis_bundle.py` или `python3 tools/make_axis_bundle.py`
   с pip-модулем bpy — файл детерминирован, числа совпадут бит-в-бит.)

2. В проекте UE 5.7 включить плагины (Edit → Plugins, после включения —
   перезапуск редактора):
     * **Python Editor Script Plugin** — обязательно;
     * **Editor Scripting Utilities** — обязательно (спавн актора, работа с ассетами);
     * **Procedural Mesh Component** — нужен для чтения вершин статик-меша
       (`unreal.ProceduralMeshLibrary.get_section_from_static_mesh`). Если плагин
       не включён, скрипт скажет об этом явным текстом и остановится.

3. Прописать путь к файлу в переменной `FBX_PATH` внизу этого блока (прямые слэши
   или сырая строка `r"..."`). При необходимости поменять `DEST_PACKAGE_PATH`.

4. Запустить: Output Log → переключить строку ввода в режим `Cmd`/`Python`
   и выполнить

       py "C:/путь/к/репозиторию/tools/ue_axis_check.py"

   (равноценно: Tools → Execute Python Script…). Весь вывод — в Output Log.

5. Результат: строка `RESULT: PASS` / `RESULT: FAIL`, ожидаемая и фактическая
   позиции, дельта по осям и её модуль. Числа перенести в
   `docs/RISK_RESULTS.md`, раздел **R1**, в таблицу
   `| Проверка | Ожидание (Blender/§11) | Факт (UE) | Вердикт |`
   (одна строка на позицию вершины, одна — на кватернион/ротатор, если он
   расходится). Если FAIL — туда же вставить фактические числа: §11 правится по
   результату теста, схема данных при этом не меняется.

ДИАГНОСТИКА ПРИ FAIL
--------------------
Скрипт печатает совпадение отдельно в локальном пространстве меша и в мировом:
  * локальная позиция совпала, мировая — нет  → неверна формула placement (§11);
  * локальная не совпала                      → неверны настройки FBX-экспорта
                                                 или импорта (оси/единицы).
"""

import math

import unreal

# ---------------------------------------------------------------------------
# НАСТРОЙКИ (владелец правит только FBX_PATH, остальное — константы теста)
# ---------------------------------------------------------------------------

# Путь к axis_probe.fbx, который напечатал tools/make_axis_bundle.py.
# Путь к копии репозитория на вашей машине + golden/fixtures/axis/axis_probe.fbx
FBX_PATH = r"D:/mimir/MH_blender_bridge/golden/fixtures/axis/axis_probe.fbx"
DEST_PACKAGE_PATH = "/Game/MimirAxisProbe"

# --- Числа ниже посчитаны tools/make_axis_bundle.py (Blender 4.5, headless) ---
# Placement под проверкой (Blender): location (1.25, -2.5, 0.75) м,
# rotation_euler XYZ (10, 20, 30)°, scale (1, 1, 1).

# §11: pos_UE = (x*100, -y*100, z*100) от location placement'а.
ACTOR_LOCATION_CM = (125.0, 250.0, 75.0)
# §11: quat_UE = (-qx, qy, -qz, qw) от кватерниона placement'а
# (Blender w,x,y,z = 0.951549, 0.038135, 0.189308, 0.239298).
ACTOR_QUAT_XYZW = (-0.038135, 0.189308, -0.239298, 0.951549)
# Справочно, тот же поворот в виде FRotator (Pitch, Yaw, Roll), градусы.
ACTOR_ROTATOR_PYR = (-20.0, -30.0, 10.0)
ACTOR_SCALE = (1.0, 1.0, 1.0)

# Контрольная вершина window_a (индекс 8 в Blender; после импорта индекс может
# быть другим — ищем ближайшую вершину, см. find_nearest).
# Локально: (0.37, 0.11, 1.93) м → §11 → см.
EXPECTED_LOCAL_CM = (37.0, -11.0, 193.0)
# Мир: Blender matrix_world @ v = (2.233146, -2.194280, 2.427456) м → §11 → см.
EXPECTED_WORLD_CM = (223.3146, 219.4280, 242.7456)

TOLERANCE_CM = 0.1


# ---------------------------------------------------------------------------
# утилиты
# ---------------------------------------------------------------------------


def log(msg):
    print(msg)
    try:
        unreal.log(msg)
    except Exception:
        pass


def fmt(v):
    return "({:.4f}, {:.4f}, {:.4f})".format(v[0], v[1], v[2])


def as_tuple(vec):
    """unreal.Vector → (x, y, z) в обычных float."""
    return (float(vec.x), float(vec.y), float(vec.z))


def distance(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def find_nearest(points, expected):
    """Ближайшая к `expected` точка: индекс вершины после импорта не обязан
    совпадать с индексом в Blender (импортёр переупорядочивает вершины и режет
    их по нормалям/UV), поэтому сверяем геометрию, а не нумерацию."""
    best_i, best_p, best_d = -1, None, float("inf")
    for i, p in enumerate(points):
        d = distance(p, expected)
        if d < best_d:
            best_i, best_p, best_d = i, p, d
    return best_i, best_p, best_d


# ---------------------------------------------------------------------------
# шаги
# ---------------------------------------------------------------------------


def import_static_mesh(fbx_path, dest_path):
    """Автоматизированный импорт через legacy FBX factory (AssetImportTask)."""
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", fbx_path)
    task.set_editor_property("destination_path", dest_path)
    task.set_editor_property("automated", True)      # без диалога импорта
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", False)

    options = unreal.FbxImportUI()
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_as_skeletal", False)
    options.set_editor_property("import_materials", False)
    options.set_editor_property("import_textures", False)
    options.set_editor_property("import_animations", False)
    options.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_STATIC_MESH)

    mesh_data = unreal.FbxStaticMeshImportData()
    # Осевые/единичные ключи импорта. Именно этот набор пайплайн обязан
    # зафиксировать на UE-стороне — иначе проверка R1 ничего не доказывает.
    mesh_data.set_editor_property("import_uniform_scale", 1.0)
    mesh_data.set_editor_property("convert_scene", True)          # FBX(Y-up) → UE(Z-up)
    mesh_data.set_editor_property("force_front_x_axis", False)
    mesh_data.set_editor_property("convert_scene_unit", False)    # геометрия уже в см
    mesh_data.set_editor_property("combine_meshes", True)
    mesh_data.set_editor_property("transform_vertex_to_absolute", True)
    mesh_data.set_editor_property("bake_pivot_in_vertex", False)
    mesh_data.set_editor_property("build_nanite", False)
    options.set_editor_property("static_mesh_import_data", mesh_data)

    task.set_editor_property("options", options)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])

    paths = list(task.get_editor_property("imported_object_paths") or [])
    log("  импортировано ассетов: {} -> {}".format(len(paths), paths))
    for path in paths:
        asset = unreal.load_asset(path.split(".")[0])
        if isinstance(asset, unreal.StaticMesh):
            return asset
    raise RuntimeError(
        "Импорт не дал ни одного StaticMesh. Проверьте FBX_PATH ({}) и то, что файл "
        "доступен редактору. Пути импортированных объектов: {}".format(fbx_path, paths))


def spawn_actor(static_mesh, transform):
    """Спавн StaticMeshActor + установка §11-конвертированного трансформа."""
    actor = None
    try:
        subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        actor = subsystem.spawn_actor_from_object(static_mesh, transform.translation)
    except Exception as ex:  # UE < 5.0 / другое имя подсистемы
        log("  EditorActorSubsystem недоступен ({}), пробую EditorLevelLibrary".format(ex))
        actor = unreal.EditorLevelLibrary.spawn_actor_from_object(
            static_mesh, transform.translation)
    if actor is None:
        raise RuntimeError(
            "Не удалось заспавнить актора. Нужен открытый уровень и включённый плагин "
            "Editor Scripting Utilities.")
    actor.set_actor_transform(transform, False, False)
    return actor


def read_vertices(static_mesh, lod_index=0):
    """Вершины статик-меша в ЛОКАЛЬНОМ пространстве (см), LOD0, все секции.

    Основной путь — ProceduralMeshLibrary.get_section_from_static_mesh (требует
    плагин Procedural Mesh Component). Если API отличается — падаем с внятным
    текстом, а не с трейсбеком внутри движка.
    """
    if not hasattr(unreal, "ProceduralMeshLibrary"):
        raise RuntimeError(
            "unreal.ProceduralMeshLibrary отсутствует: включите плагин "
            "'Procedural Mesh Component' (Edit → Plugins) и перезапустите редактор.")

    try:
        num_sections = static_mesh.get_num_sections(lod_index)
    except Exception as ex:
        log("  get_num_sections недоступен ({}), считаю, что секция одна".format(ex))
        num_sections = 1

    points = []
    for section in range(max(1, num_sections)):
        try:
            result = unreal.ProceduralMeshLibrary.get_section_from_static_mesh(
                static_mesh, lod_index, section)
        except Exception as ex:
            raise RuntimeError(
                "get_section_from_static_mesh(LOD {}, секция {}) не отработал: {}. "
                "Проверьте, что включён плагин Procedural Mesh Component и что у меша "
                "есть CPU-доступная геометрия (Allow CPUAccess / не Nanite-only).".format(
                    lod_index, section, ex))
        # Возвращается кортеж (vertices, triangles, normals, uvs, tangents).
        vertices = result[0] if isinstance(result, (tuple, list)) else result
        points.extend(as_tuple(v) for v in vertices)

    if not points:
        raise RuntimeError("У импортированного меша не прочиталось ни одной вершины.")
    return points


def to_world(transform, point):
    """Локальная точка → мир. transform_location, с ручным запасным путём."""
    vec = unreal.Vector(point[0], point[1], point[2])
    try:
        return as_tuple(transform.transform_location(vec))
    except Exception as ex:
        log("  Transform.transform_location недоступен ({}), считаю вручную".format(ex))
        x, y, z, w = ACTOR_QUAT_XYZW
        px, py, pz = (point[i] * ACTOR_SCALE[i] for i in range(3))
        # v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
        tx = 2.0 * (y * pz - z * py)
        ty = 2.0 * (z * px - x * pz)
        tz = 2.0 * (x * py - y * px)
        rx = px + w * tx + (y * tz - z * ty)
        ry = py + w * ty + (z * tx - x * tz)
        rz = pz + w * tz + (x * ty - y * tx)
        return (rx + ACTOR_LOCATION_CM[0],
                ry + ACTOR_LOCATION_CM[1],
                rz + ACTOR_LOCATION_CM[2])


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main():
    log("=" * 78)
    log("R1 axis check (Mimir Composite Pipeline, §11) - UE side")
    log("=" * 78)
    log("  FBX          : {}".format(FBX_PATH))
    log("  назначение   : {}".format(DEST_PACKAGE_PATH))
    log("  ожидание     : локально {} см, мир {} см".format(
        fmt(EXPECTED_LOCAL_CM), fmt(EXPECTED_WORLD_CM)))
    log("  допуск       : {} см".format(TOLERANCE_CM))

    static_mesh = import_static_mesh(FBX_PATH, DEST_PACKAGE_PATH)
    log("  StaticMesh   : {}".format(static_mesh.get_path_name()))

    transform = unreal.Transform()
    transform.set_editor_property(
        "translation", unreal.Vector(*ACTOR_LOCATION_CM))
    transform.set_editor_property(
        "rotation", unreal.Quat(*ACTOR_QUAT_XYZW))
    transform.set_editor_property("scale3d", unreal.Vector(*ACTOR_SCALE))

    actor = spawn_actor(static_mesh, transform)
    log("  актор        : {}".format(actor.get_actor_label()))
    log("  трансформ    : location {} см, quat (x,y,z,w) = ({:.6f}, {:.6f}, {:.6f}, {:.6f}), "
        "scale {}".format(fmt(ACTOR_LOCATION_CM), ACTOR_QUAT_XYZW[0], ACTOR_QUAT_XYZW[1],
                          ACTOR_QUAT_XYZW[2], ACTOR_QUAT_XYZW[3], fmt(ACTOR_SCALE)))
    log("  rotator (справочно, ожидаемый P/Y/R): {}".format(fmt(ACTOR_ROTATOR_PYR)))
    try:
        rot = actor.get_actor_rotation()
        log("  rotator (фактический у актора)      : ({:.4f}, {:.4f}, {:.4f})".format(
            rot.pitch, rot.yaw, rot.roll))
    except Exception as ex:
        log("  get_actor_rotation недоступен: {}".format(ex))

    local_points = read_vertices(static_mesh)
    log("  вершин LOD0  : {}".format(len(local_points)))

    # 1) локальная сверка: правильные ли оси/единицы принёс FBX
    li, lp, ld = find_nearest(local_points, EXPECTED_LOCAL_CM)
    log("-" * 78)
    log("  ЛОКАЛЬНО: ожидание {}".format(fmt(EXPECTED_LOCAL_CM)))
    log("            ближайшая вершина #{} = {}".format(li, fmt(lp)))
    log("            дельта {} , |дельта| = {:.4f} см".format(
        fmt([lp[i] - EXPECTED_LOCAL_CM[i] for i in range(3)]), ld))
    local_ok = ld <= TOLERANCE_CM

    # 2) мировая сверка: правильна ли формула placement §11
    world_points = [to_world(transform, p) for p in local_points]
    wi, wp, wd = find_nearest(world_points, EXPECTED_WORLD_CM)
    log("  МИР     : ожидание {}".format(fmt(EXPECTED_WORLD_CM)))
    log("            ближайшая вершина #{} = {}".format(wi, fmt(wp)))
    log("            дельта {} , |дельта| = {:.4f} см".format(
        fmt([wp[i] - EXPECTED_WORLD_CM[i] for i in range(3)]), wd))
    world_ok = wd <= TOLERANCE_CM

    log("-" * 78)
    log("RESULT: {}".format("PASS" if (local_ok and world_ok) else "FAIL"))
    if not local_ok:
        log("  ЛОКАЛЬНАЯ сверка не прошла → расходятся настройки FBX-экспорта/импорта "
            "(оси или единицы), а не формула §11.")
    if local_ok and not world_ok:
        log("  Локально совпало, в мире — нет → неверна формула placement §11 "
            "(pos/quat). Впишите фактические числа в docs/RISK_RESULTS.md, R1.")
    log("  Числа → docs/RISK_RESULTS.md, раздел R1, таблица "
        "'| Проверка | Ожидание (Blender/§11) | Факт (UE) | Вердикт |'.")
    log("=" * 78)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # понятный текст вместо трейсбека движка
        log("RESULT: FAIL (скрипт не отработал)")
        log("  причина: {}".format(error))
        raise
