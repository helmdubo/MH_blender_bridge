import bpy
import json
import shutil
import os
from collections import deque
import contextlib
import bmesh
import re
from mathutils import Matrix

original_material_names = {}
new_material_names = {}
_BLENDER_DUP_MATERIAL_RE = re.compile(r"^(.*?)([._]\d{1,4})$")
TEXTURE_ROOT_MARKER = "EnlistedCDK"
TEXTURE_FILE_EXTENSIONS = (
    ".dds",
    ".tga",
    ".png",
    ".jpg",
    ".jpeg",
    ".tif",
    ".tiff",
    ".bmp",
    ".psd",
    ".exr",
    ".hdr",
)
BLENDER_METERS_TO_UE_CENTIMETERS = 100.0

def make_export_cache():
    return {
        "material_properties": {},
        "material_texture_paths": {},
        "copied_material_textures": set(),
        "texture_sources": {},
    }

def ensure_folder_exists(folder_path):
    if not os.path.exists(folder_path):
        os.makedirs(folder_path, exist_ok=True)
    return folder_path

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
def _temporary_ue_centimeter_export_state(objects):
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
        object_states.append((obj, obj.matrix_parent_inverse.copy(), obj.matrix_basis.copy()))
        if obj.type == 'MESH' and obj.data is not None and obj.data not in seen_mesh_data:
            seen_mesh_data.add(obj.data)
            mesh_data.append(obj.data)

    scale = BLENDER_METERS_TO_UE_CENTIMETERS
    scale_matrix = Matrix.Scale(scale, 4)
    inverse_scale_matrix = Matrix.Scale(1.0 / scale, 4)
    scaled_mesh_data = []

    try:
        # UE stores asset geometry and component locations in centimeters.
        # Scale only geometry and translation parts; object/local scales stay intact.
        for mesh in mesh_data:
            _transform_mesh_data(mesh, scale_matrix)
            scaled_mesh_data.append(mesh)
        for obj, matrix_parent_inverse, matrix_basis in object_states:
            obj.matrix_parent_inverse = _scale_matrix_translation(matrix_parent_inverse, scale)
            obj.matrix_basis = _scale_matrix_translation(matrix_basis, scale)

        unit_settings.system = 'METRIC'
        unit_settings.scale_length = 0.01
        try:
            unit_settings.length_unit = 'CENTIMETERS'
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

def print_custom_properties(struct):
    properties = {}
    for prop_name in struct.keys():
        prop_value = struct[prop_name]
        if isinstance(prop_value, (bpy.types.bpy_prop_array, list, tuple)):
            values = [prop_value[i] for i in range(len(prop_value))]
            properties[prop_name] = values
        elif isinstance(prop_value, (float, int, str)):
            properties[prop_name] = prop_value
        else:
            try:
                values = [prop_value[i] for i in range(len(prop_value))]
                properties[prop_name] = values
            except TypeError:
                properties[prop_name] = prop_value
    return properties

def collect_material_properties(material):
    result = {
        'shader_class': None,
        'sides': None,
        'optional': {},
        'textures': {}
    }

    if hasattr(material, "dagormat"):
        dagormat = material.dagormat

        # Collect shader_class and sides
        properties = print_properties_with_sublevels(dagormat, max_depth=1)
        result['shader_class'] = properties.get('shader_class')
        result['sides'] = properties.get('sides')

        # Collect optional properties
        if hasattr(dagormat, "optional"):
            result['optional'] = print_custom_properties(dagormat.optional)

        # Collect texture properties
        if hasattr(dagormat, "textures"):
            result['textures'] = print_custom_properties(dagormat.textures)

    return result

def get_material_properties_cached(material, export_cache=None):
    if export_cache is None:
        return collect_material_properties(material)

    material_cache = export_cache.setdefault("material_properties", {})
    if material not in material_cache:
        material_cache[material] = collect_material_properties(material)
    return material_cache[material]

def print_properties_with_sublevels(struct, max_depth=1):
    queue = deque([(struct, 0)])
    visited = set()
    result = {}

    while queue:
        current_struct, depth = queue.popleft()
        if depth > max_depth:
            continue

        for prop_name in dir(current_struct):
            if prop_name.startswith("_"):
                continue

            prop_value = getattr(current_struct, prop_name)
            if isinstance(prop_value, (float, int, str, bool, bpy.types.bpy_prop_array)):
                result[prop_name] = prop_value
            elif hasattr(prop_value, "keys") and prop_name not in visited:
                visited.add(prop_name)
                queue.append((prop_value, depth + 1))
    return result

def _clean_texture_path(texture_path):
    if not isinstance(texture_path, str):
        return ""
    return os.path.expandvars(texture_path.strip().strip('"').strip("'"))

def _texture_relative_path_from_cdk(texture_path):
    cleaned = _clean_texture_path(texture_path)
    if not cleaned:
        return None

    parts = [part for part in cleaned.replace("\\", "/").split("/") if part and part != "."]
    marker_index = None
    for index, part in enumerate(parts):
        if part.lower() == TEXTURE_ROOT_MARKER.lower():
            marker_index = index
            break

    if marker_index is None:
        return None

    relative_parts = parts[marker_index:]
    if len(relative_parts) < 2 or any(part == ".." for part in relative_parts):
        return None
    return os.path.join(*relative_parts)

def _cdk_parent_from_path(path):
    if not path:
        return None

    normalized = os.path.abspath(path).replace("\\", "/")
    lower_path = normalized.lower()
    marker = "/" + TEXTURE_ROOT_MARKER.lower() + "/"
    marker_index = lower_path.find(marker)
    if marker_index != -1:
        return normalized[:marker_index]

    marker_suffix = "/" + TEXTURE_ROOT_MARKER.lower()
    if lower_path.endswith(marker_suffix):
        return normalized[:-len(marker_suffix)]

    return None

def _deduplicate_paths(paths):
    seen = set()
    for path in paths:
        if not path:
            continue
        normalized = os.path.normcase(os.path.abspath(os.path.normpath(path)))
        if normalized in seen:
            continue
        seen.add(normalized)
        yield path

def _texture_source_candidates(texture_path):
    cleaned = _clean_texture_path(texture_path)
    if not cleaned:
        return []

    candidates = []
    if cleaned.startswith("//"):
        candidates.append(bpy.path.abspath(cleaned))
    else:
        candidates.append(cleaned)

    relative_cdk_path = _texture_relative_path_from_cdk(cleaned)
    blend_path = getattr(bpy.data, "filepath", "")
    blend_folder = os.path.dirname(blend_path) if blend_path else ""

    if blend_folder:
        if not os.path.isabs(cleaned):
            candidates.append(os.path.join(blend_folder, cleaned))
        if relative_cdk_path:
            cdk_parent = _cdk_parent_from_path(blend_folder)
            if cdk_parent:
                candidates.append(os.path.join(cdk_parent, relative_cdk_path))
            candidates.append(os.path.join(blend_folder, relative_cdk_path))

    return list(_deduplicate_paths(candidates))

def _texture_file_variants(path):
    yield path
    root, extension = os.path.splitext(path)
    if not extension:
        for texture_extension in TEXTURE_FILE_EXTENSIONS:
            yield root + texture_extension

def _resolve_existing_texture_path(texture_path):
    for candidate_path in _texture_source_candidates(texture_path):
        for file_path in _texture_file_variants(candidate_path):
            normalized_path = os.path.normpath(file_path)
            if os.path.isfile(normalized_path):
                return normalized_path
    return None

def _iter_texture_paths_from_value(value):
    if isinstance(value, str):
        cleaned = _clean_texture_path(value)
        if cleaned:
            yield cleaned
    elif isinstance(value, dict):
        for item in value.values():
            yield from _iter_texture_paths_from_value(item)
    elif isinstance(value, (list, tuple)):
        for item in value:
            yield from _iter_texture_paths_from_value(item)

def _iter_material_texture_paths(material, shader_info):
    if shader_info:
        yield from _iter_texture_paths_from_value(shader_info.get("textures", {}))

    if not (material and material.use_nodes and material.node_tree):
        return

    for node in material.node_tree.nodes:
        if node.type != "TEX_IMAGE" or not node.image:
            continue
        for image_path in (node.image.filepath, getattr(node.image, "filepath_raw", "")):
            cleaned = _clean_texture_path(image_path)
            if cleaned:
                yield cleaned

def copy_export_texture(texture_path, dest_folder, copied_textures, export_cache=None):
    cleaned_texture_path = _clean_texture_path(texture_path)
    if not cleaned_texture_path:
        return False

    request_key = ("request", os.path.normcase(cleaned_texture_path.replace("\\", "/")))
    if request_key in copied_textures:
        return True
    copied_textures.add(request_key)

    relative_path = _texture_relative_path_from_cdk(cleaned_texture_path)
    if not relative_path:
        print(f"Texture skipped, no {TEXTURE_ROOT_MARKER} path marker: {cleaned_texture_path}")
        return False

    source_cache = export_cache.setdefault("texture_sources", {}) if export_cache is not None else None
    source_key = os.path.normcase(cleaned_texture_path.replace("\\", "/"))
    if source_cache is not None and source_key in source_cache:
        source_path = source_cache[source_key]
    else:
        source_path = _resolve_existing_texture_path(cleaned_texture_path)
        if source_cache is not None:
            source_cache[source_key] = source_path

    if not source_path:
        print(f"Texture source not found: {cleaned_texture_path}")
        return False

    source_extension = os.path.splitext(source_path)[1]
    if source_extension and not os.path.splitext(relative_path)[1]:
        relative_path += source_extension

    dest_path = os.path.normpath(os.path.join(dest_folder, relative_path))
    dest_root = os.path.abspath(dest_folder)
    dest_abs = os.path.abspath(dest_path)
    try:
        if os.path.commonpath([dest_root, dest_abs]) != dest_root:
            print(f"Texture skipped, destination is outside export folder: {dest_abs}")
            return False
    except ValueError:
        print(f"Texture skipped, invalid destination path: {dest_abs}")
        return False

    copy_key = (
        "copy",
        os.path.normcase(os.path.abspath(source_path)),
        os.path.normcase(dest_abs),
    )
    if copy_key in copied_textures:
        return True

    ensure_folder_exists(os.path.dirname(dest_path))
    if os.path.normcase(os.path.abspath(source_path)) != os.path.normcase(dest_abs):
        shutil.copy2(source_path, dest_path)
        print(f"Texture copied: {source_path} -> {dest_path}")
    else:
        print(f"Texture already in export folder: {dest_path}")

    copied_textures.add(copy_key)
    return True

def copy_material_texture_maps(material, shader_info, dest_folder, copied_textures, export_cache=None):
    if export_cache is not None:
        copied_materials = export_cache.setdefault("copied_material_textures", set())
        if material in copied_materials:
            return
        copied_materials.add(material)

        texture_paths_cache = export_cache.setdefault("material_texture_paths", {})
        if material in texture_paths_cache:
            texture_paths = texture_paths_cache[material]
        else:
            texture_paths = tuple(dict.fromkeys(_iter_material_texture_paths(material, shader_info)))
            texture_paths_cache[material] = texture_paths
    else:
        texture_paths = tuple(dict.fromkeys(_iter_material_texture_paths(material, shader_info)))

    for texture_path in texture_paths:
        copy_export_texture(texture_path, dest_folder, copied_textures, export_cache)

def deduplicate_material_slots(obj):
    """Ищет дубликаты материалов (с суффиксами .001, _01 и т.д.) и заменяет их на оригинал, если он существует."""
    fixed_count = 0
    
    for slot in obj.material_slots:
        if not slot.material:
            continue
        
        current_name = slot.material.name
        base_name = current_name
        match = _BLENDER_DUP_MATERIAL_RE.match(base_name)
        
        while match:
            potential_base = match.group(1)
            if potential_base in bpy.data.materials:
                base_name = potential_base
                match = _BLENDER_DUP_MATERIAL_RE.match(base_name)
            else:
                break
        
        if base_name != current_name and base_name in bpy.data.materials:
            slot.material = bpy.data.materials[base_name]
            fixed_count += 1
            
    return fixed_count

def rename_materials(obj):
    """Переименовывает материалы объекта, заменяя '#' и пробелы на '_', и добавляя префикс 'm_'."""
    global original_material_names, new_material_names
    for mat_slot in obj.material_slots:
        mat = mat_slot.material
        if not mat:
            continue
        if mat in new_material_names:
            target_name = new_material_names[mat]
            if mat.name != target_name:
                mat.name = target_name
            continue

        original_name = mat.name
        if mat not in original_material_names:
            original_material_names[mat] = original_name
        new_name = original_name.replace('#', '').replace(' ', '').replace('*', '_').replace('.', '_')
        if not new_name.startswith("m_"):
            new_name = "m_" + new_name
        new_material_names[mat] = new_name
        if mat.name != new_name:
            mat.name = new_name

def restore_material_names():
    """Восстанавливает оригинальные имена материалов объектов."""
    global original_material_names, new_material_names
    restored_count = 0
    for mat, original_name in original_material_names.items():
        if mat.name != original_name:
            mat.name = original_name
            restored_count += 1
    if restored_count:
        print(f"Restored {restored_count} material name(s).")
    original_material_names.clear()
    new_material_names.clear()

def clean_custom_properties(obj):
    protected_keys = {"_RNA_UI", "cycles"}
    keys_to_delete = [key for key in obj.keys() if key not in protected_keys]
    for key in keys_to_delete:
        del obj[key]

def remove_export_metadata(processed_objects):
    """Удаляет временные метаданные (кастомные свойства) с объектов после экспорта."""
    print("Cleaning up temporary export metadata...")
    for obj in processed_objects:
        if obj and obj.type == 'MESH':
            clean_custom_properties(obj)

def process_object_hierarchy(obj, processed_objects, dest_folder, copied_textures, export_cache=None, copy_textures=False):
    if obj in processed_objects:
        return
    processed_objects.add(obj)

    if obj.type == 'MESH':
        # Очистка дубликатов перед переименованием
        deduplicate_material_slots(obj)
        # Переименование материалов перед обработкой
        rename_materials(obj)
        clean_custom_properties(obj)
        for mat_slot in obj.material_slots:
            mat = mat_slot.material
            if mat:
                shader_info = get_material_properties_cached(mat, export_cache)
                if copy_textures:
                    copy_material_texture_maps(mat, shader_info, dest_folder, copied_textures, export_cache)
                if not mat.use_nodes:
                    continue
                clean_material_name = mat.name  # Используем обновленное имя материала
                obj[clean_material_name] = json.dumps(shader_info, ensure_ascii=False)

    for child in obj.children:
        process_object_hierarchy(child, processed_objects, dest_folder, copied_textures, export_cache, copy_textures)

def check_and_fix_duplicate_material_names(processed_objects=None):
    """Проверяет и исправляет дублирующиеся имена материалов с разным регистром."""
    global new_material_names
    seen_names = {}
    duplicates = {}

    # Проверяем на дубликаты
    for mat, new_name in new_material_names.items():
        lower_name = new_name.lower()
        if lower_name in seen_names:
            duplicates[mat] = new_name
        else:
            seen_names[lower_name] = mat

    target_objects = processed_objects if processed_objects is not None else bpy.context.scene.objects

    # Исправляем дубликаты
    for mat, new_name in duplicates.items():
        fixed_name = new_name + "_fix"
        new_material_names[mat] = fixed_name
        mat.name = fixed_name
        # Обновляем метаданные
        for obj in target_objects:
            if new_name in obj.keys():
                shader_info_json = obj[new_name]
                del obj[new_name]
                obj[fixed_name] = shader_info_json


# --- Collapse Empty -> Merge direct child meshes (bottom-up) -----------------

def _get_view3d_override(context):
    """Return an override dict for VIEW_3D ops, or None if no 3D View exists."""
    try:
        screen = context.window.screen
    except Exception:
        return None
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            region = next((r for r in area.regions if r.type == 'WINDOW'), None)
            if region:
                return {
                    "window": context.window,
                    "area": area,
                    "region": region,
                    "screen": screen,
                }
    return None

def _get_selected_collections_any(context, include_active_layer_fallback=True):
    """Return selected bpy.types.Collection(s), even when the operator is run from a non-Outliner area."""
    try:
        cols = [obj for obj in getattr(bpy.context, "selected_ids", []) if isinstance(obj, bpy.types.Collection)]
        if cols:
            return cols
    except Exception:
        pass

    try:
        win = getattr(context, "window", None)
        screen = getattr(win, "screen", None) if win else getattr(context, "screen", None)
        if screen:
            for area in screen.areas:
                if area.type == 'OUTLINER':
                    region = next((r for r in area.regions if r.type == 'WINDOW'), None)
                    if region is None:
                        continue
                    with bpy.context.temp_override(window=win, screen=screen, area=area, region=region):
                        ids = getattr(bpy.context, "selected_ids", [])
                        cols2 = [obj for obj in ids if isinstance(obj, bpy.types.Collection)]
                        if cols2:
                            return cols2
                        active_id = getattr(bpy.context, "active_id", None)
                        if isinstance(active_id, bpy.types.Collection):
                            return [active_id]
    except Exception:
        pass

    try:
        active_id = getattr(bpy.context, "active_id", None)
        if isinstance(active_id, bpy.types.Collection):
            return [active_id]
    except Exception:
        pass

    if include_active_layer_fallback:
        try:
            alc = getattr(getattr(context, "view_layer", None), "active_layer_collection", None)
            if alc and isinstance(getattr(alc, "collection", None), bpy.types.Collection):
                return [alc.collection]
        except Exception:
            pass

    return []

def _get_selection_roots(context):
    sel_objs = list(getattr(context, "selected_objects", []) or [])
    if sel_objs:
        sel_set = set(sel_objs)
        roots = [o for o in sel_objs if (o.parent is None or o.parent not in sel_set)]
        return ("objects", roots)

    cols = _get_selected_collections_any(context, include_active_layer_fallback=True)
    if cols:
        return ("collections", cols)

    return ("none", [])

def _object_depth(obj):
    d = 0
    p = obj.parent
    while p is not None:
        d += 1
        p = p.parent
    return d

def _collect_subtree(roots):
    stack = list(roots)
    seen = set()
    out = []
    while stack:
        obj = stack.pop()
        if obj in seen:
            continue
        seen.add(obj)
        out.append(obj)
        stack.extend(list(obj.children))
    return out

def _link_object_to_collections(obj, collections, fallback_collection):
    cols = [c for c in collections if c is not None]
    if not cols:
        cols = [fallback_collection]
    for c in cols:
        if obj.name not in c.objects:
            c.objects.link(obj)

def _cleanup_orphan_mesh(mesh):
    try:
        if mesh and mesh.users == 0:
            bpy.data.meshes.remove(mesh)
    except Exception:
        pass

def _collapse_one_empty(empty_obj, depsgraph, context, universe_set=None):
    if empty_obj is None or empty_obj.type != 'EMPTY':
        return None

    direct_mesh_children = [c for c in empty_obj.children if c.type == 'MESH']
    if universe_set is not None:
        direct_mesh_children = [c for c in direct_mesh_children if c in universe_set]

    if not direct_mesh_children:
        return None

    override = _get_view3d_override(context)
    if override is None:
        raise RuntimeError("No VIEW_3D area found. Open a 3D Viewport and try again.")

    empty_world = empty_obj.matrix_world.copy()
    empty_parent = empty_obj.parent
    empty_parent_inverse = empty_obj.matrix_parent_inverse.copy()

    empty_collections = list(getattr(empty_obj, "users_collection", []))
    fallback_collection = context.scene.collection

    target_name = empty_obj.name
    base_mesh = bpy.data.meshes.new(name=f"{target_name}__MERGED_MESH")
    base_obj = bpy.data.objects.new(name=f"{target_name}__MERGED_OBJ", object_data=base_mesh)
    _link_object_to_collections(base_obj, empty_collections, fallback_collection)
    base_obj.matrix_world = empty_world

    tmp_mesh_datas = []
    tmp_objs = []
    for src in direct_mesh_children:
        src_eval = src.evaluated_get(depsgraph)
        try:
            tmp_mesh = bpy.data.meshes.new_from_object(
                src_eval,
                preserve_all_data_layers=True,
                depsgraph=depsgraph
            )
        except TypeError:
            tmp_mesh = bpy.data.meshes.new_from_object(src_eval, depsgraph=depsgraph)

        tmp_obj = bpy.data.objects.new(name=f"__TMP__{src.name}", object_data=tmp_mesh)
        _link_object_to_collections(tmp_obj, empty_collections[:1], fallback_collection)
        tmp_obj.matrix_world = src.matrix_world.copy()

        tmp_mesh_datas.append(tmp_mesh)
        tmp_objs.append(tmp_obj)

    prev_selected = list(context.selected_objects)
    prev_active = context.view_layer.objects.active

    with bpy.context.temp_override(**override):
        if context.mode != 'OBJECT':
            try:
                bpy.ops.object.mode_set(mode='OBJECT')
            except Exception:
                pass

        bpy.ops.object.select_all(action='DESELECT')
        base_obj.select_set(True)
        context.view_layer.objects.active = base_obj
        for o in tmp_objs:
            o.select_set(True)

        bpy.ops.object.join()
        bpy.ops.object.select_all(action='DESELECT')

    for m in tmp_mesh_datas:
        _cleanup_orphan_mesh(m)

    children_to_move = [c for c in list(empty_obj.children) if c not in direct_mesh_children]
    for src in direct_mesh_children:
        children_to_move.extend(list(src.children))

    base_obj.parent = empty_parent
    base_obj.matrix_parent_inverse = empty_parent_inverse
    base_obj.matrix_world = empty_world

    for ch in children_to_move:
        if ch is None:
            continue
        world = ch.matrix_world.copy()
        ch.parent = base_obj
        ch.matrix_world = world

    bpy.data.objects.remove(empty_obj, do_unlink=True)

    for src in direct_mesh_children:
        if src.name in bpy.data.objects:
            bpy.data.objects.remove(src, do_unlink=True)

    base_obj.name = target_name
    base_obj.data.name = target_name

    try:
        for o in prev_selected:
            if o and o.name in bpy.data.objects:
                bpy.data.objects[o.name].select_set(True)
        if prev_active and prev_active.name in bpy.data.objects:
            context.view_layer.objects.active = bpy.data.objects[prev_active.name]
    except Exception:
        pass

    return base_obj

def collapse_empty_mesh_groups(context, roots, traverse_children=True, universe_objects=None):
    depsgraph = context.evaluated_depsgraph_get()

    if traverse_children:
        universe = _collect_subtree(roots)
        universe_set = set(universe)
    else:
        universe = list(universe_objects) if universe_objects is not None else list(roots)
        universe_set = set(universe)

    empties = [o for o in universe if o.type == 'EMPTY']
    empties.sort(key=_object_depth, reverse=True)

    collapsed_count = 0
    for e in empties:
        if e is None:
            continue
        if e.name not in bpy.data.objects:
            continue
        new_obj = _collapse_one_empty(bpy.data.objects[e.name], depsgraph, context, universe_set=universe_set)
        if new_obj is not None:
            collapsed_count += 1

    return collapsed_count

class OBJECT_OT_CollapseEmptyMeshGroups(bpy.types.Operator):
    bl_idname = "object.collapse_empty_mesh_groups"
    bl_label = "Collapse Empty -> Merge Mesh Children"
    bl_description = "If an EMPTY has direct MESH children, merge them and replace the EMPTY (bottom-up)."
    bl_options = {'REGISTER', 'UNDO'}
    def execute(self, context):
        sel_mode, selection = _get_selection_roots(context)
        try:
            if sel_mode == "objects":
                roots = list(selection)
                total = collapse_empty_mesh_groups(context, roots=roots, traverse_children=True)
                self.report({'INFO'}, f"Collapsed {total} empty node(s) in selected object(s).")
            elif sel_mode == "collections":
                total = 0
                for collection in selection:
                    all_objs = list(collection.all_objects)
                    total += collapse_empty_mesh_groups(context, roots=all_objs, traverse_children=False, universe_objects=all_objs)
                self.report({'INFO'}, f"Collapsed {total} empty node(s) in selected collection(s).")
            else:
                self.report({'WARNING'}, "No selected objects or collections found.")
        except Exception as ex:
            self.report({'ERROR'}, f"Collapse failed: {ex}")
        return {'FINISHED'}

def select_object_and_children(obj, selected_objects):
    if obj not in selected_objects:
        obj.select_set(True)
        selected_objects.add(obj)
        for child in obj.children:
            select_object_and_children(child, selected_objects)

def export_fbx_for_objects(objects, dest_folder, file_name):
    bpy.ops.object.select_all(action='DESELECT')
    selected_objects = set()
    for obj in objects:
        select_object_and_children(obj, selected_objects)
    export_path = os.path.join(dest_folder, f"{file_name}.fbx")
    
    # Считываем режим экспорта из сцены
    export_mode = getattr(bpy.context.scene, "export_mode", 'STATIC')
    
    # Для скелетного режима НЕЛЬЗЯ удалять недеформирующие кости
    only_deform = False if export_mode == 'SKELETAL' else True

    with _temporary_ue_centimeter_export_state(selected_objects):
        bpy.ops.export_scene.fbx(
            filepath=export_path,
            use_selection=True,
            use_custom_props=True,
            use_mesh_modifiers=True,
            
            apply_unit_scale=True,                # Говорит FBX записать правильные единицы измерения
            # The temporary export state already converts geometry and locations
            # to centimeters, so Blender must not push a 100x scale into nodes.
            apply_scale_options='FBX_SCALE_UNITS',
            global_scale=1.0,                     # Больше никаких искусственных умножений
            # -----------------------------------
            
            # Keep Blender's default FBX scene-transform behavior. Baking the axis
            # transform into object data can break scene hierarchy/instance import
            # in UE FBX Import Scene.
            use_space_transform=True,
            bake_space_transform=False,
            add_leaf_bones=False,
            use_armature_deform_only=only_deform,
            axis_forward='X',
            axis_up='Z'
        )
    bpy.ops.object.select_all(action='DESELECT')

def _collect_texture_export_scope_objects(context):
    sel_mode, selection = _get_selection_roots(context)
    if sel_mode == "objects":
        return _collect_subtree(list(selection))

    if sel_mode == "collections":
        objects = []
        seen = set()
        for collection in selection:
            for obj in collection.all_objects:
                if obj in seen:
                    continue
                seen.add(obj)
                objects.append(obj)
        return objects

    return []

def export_texture_maps_for_current_selection(dest_folder, copied_textures, export_cache=None):
    if export_cache is None:
        export_cache = make_export_cache()

    objects = _collect_texture_export_scope_objects(bpy.context)
    processed_objects = set()
    processed_materials = set()

    for obj in objects:
        if not obj or obj in processed_objects or obj.name not in bpy.data.objects:
            continue
        processed_objects.add(obj)
        if obj.type != 'MESH':
            continue

        for mat_slot in obj.material_slots:
            mat = mat_slot.material
            if not mat:
                continue
            processed_materials.add(mat)
            shader_info = get_material_properties_cached(mat, export_cache)
            copy_material_texture_maps(mat, shader_info, dest_folder, copied_textures, export_cache)

    processed_texture_count = sum(
        1 for key in copied_textures
        if isinstance(key, tuple) and len(key) > 0 and key[0] == "copy"
    )
    return len(processed_objects), len(processed_materials), processed_texture_count

def list_materials_and_shader_groups_in_selected_collections(dest_folder, copied_textures, export_cache=None):
    if export_cache is None:
        export_cache = make_export_cache()

    sel_mode, selection = _get_selection_roots(bpy.context)
    selected_collections = selection if sel_mode == "collections" else []

    do_collapse = getattr(bpy.context.scene, "collapse_empty_mesh_groups", False)
    if do_collapse:
        if sel_mode == "collections":
            for collection in selected_collections:
                all_objs = list(collection.all_objects)
                collapse_empty_mesh_groups(bpy.context, roots=all_objs, traverse_children=False, universe_objects=all_objs)
        elif sel_mode == "objects":
            roots = list(selection)
            if roots:
                collapse_empty_mesh_groups(bpy.context, roots=roots, traverse_children=True)

        # Collapse can replace selected EMPTY roots with newly-created MESH objects.
        # Re-read selection before optional decal split/export so the next step does
        # not operate on removed object references.
        sel_mode, selection = _get_selection_roots(bpy.context)
        selected_collections = selection if sel_mode == "collections" else []

    do_align_ucx_pivots = getattr(bpy.context.scene, "align_ucx_collision_pivots", False)
    if do_align_ucx_pivots:
        if sel_mode == "collections":
            for collection in selected_collections:
                all_objs = list(collection.all_objects)
                align_ucx_collision_pivots(bpy.context, roots=all_objs, traverse_children=False, universe_objects=all_objs)
        elif sel_mode == "objects":
            roots = list(selection)
            if roots:
                align_ucx_collision_pivots(bpy.context, roots=roots, traverse_children=True)

    do_split_decals = getattr(bpy.context.scene, "split_decal_faces", False)
    if do_split_decals:
        if sel_mode == "collections":
            for collection in selected_collections:
                all_objs = list(collection.all_objects)
                separate_decal_faces_bottom_up(bpy.context, roots=all_objs, traverse_children=False, universe_objects=all_objs, export_cache=export_cache)
        elif sel_mode == "objects":
            roots = list(selection)
            if roots:
                separate_decal_faces_bottom_up(bpy.context, roots=roots, traverse_children=True, export_cache=export_cache)

    # Определяем поддерживаемые типы объектов в зависимости от режима экспорта
    export_mode = getattr(bpy.context.scene, "export_mode", 'STATIC')
    valid_types = {'MESH', 'EMPTY'}
    if export_mode == 'SKELETAL':
        valid_types.add('ARMATURE')  # Разрешаем экспорт скелетов

    if sel_mode == "objects":
        selected_objects = list(selection)
        processed_objects = set()
        for obj in selected_objects:
            process_object_hierarchy(obj, processed_objects, dest_folder, copied_textures, export_cache)
        check_and_fix_duplicate_material_names(processed_objects)
        first_object_name = selected_objects[0].name if selected_objects else 'exported_objects'
        
        # Гарантированное восстановление имен и метаданных через try...finally
        try:
            export_fbx_for_objects(selected_objects, dest_folder, first_object_name)
        finally:
            restore_material_names()
            remove_export_metadata(processed_objects)

    elif sel_mode == "collections":
        for collection in selected_collections:
            # Фильтрация типов теперь учитывает valid_types
            all_objects = [obj for obj in collection.all_objects if obj.type in valid_types]
            processed_objects = set()
            for obj in all_objects:
                process_object_hierarchy(obj, processed_objects, dest_folder, copied_textures, export_cache)
            check_and_fix_duplicate_material_names(processed_objects)
            
            # Гарантированное восстановление имен и метаданных через try...finally
            try:
                export_fbx_for_objects(all_objects, dest_folder, collection.name)
            finally:
                restore_material_names()
                remove_export_metadata(processed_objects)
    else:
        print("No selected objects or collections found for export.")

def make_collection_instances_real(context):
    selected_ids = bpy.context.selected_ids
    selected_collections = [obj for obj in selected_ids if isinstance(obj, bpy.types.Collection)]
    
    if selected_collections:
        for collection in selected_collections:
            for obj in collection.objects:
                if obj.type == 'EMPTY' and obj.instance_type == 'COLLECTION' and obj.instance_collection:
                    obj.select_set(True)
                    context.view_layer.objects.active = obj
    else:
        root_names = [o.name for o in bpy.context.selected_objects]
        selected_objects = [bpy.data.objects.get(n) for n in root_names if bpy.data.objects.get(n)]
        for obj in selected_objects:
            if obj.type == 'EMPTY' and obj.instance_type == 'COLLECTION' and obj.instance_collection:
                obj.select_set(True)
                context.view_layer.objects.active = obj

    bpy.ops.object.duplicates_make_real(use_base_parent=True, use_hierarchy=True)
    bpy.ops.object.select_all(action='DESELECT')

def convert_to_absolute_path(relative_path):
    if relative_path.startswith("//"):
        return bpy.path.abspath(relative_path)
    return relative_path

def _unique_name(base_name, existing_names):
    if base_name not in existing_names:
        return base_name
    i = 1
    while True:
        cand = f"{base_name}.{i:03d}"
        if cand not in existing_names:
            return cand
        i += 1

# --- UCX collision pivot alignment ------------------------------------------

_UCX_PREFIX_RE = re.compile(r"^UCX_(.+)$", re.IGNORECASE)
_UCX_SUFFIX_RE = re.compile(r"^(.*)_\d{1,3}$")
_BLENDER_DUP_SUFFIX_RE = re.compile(r"^(.*)\.\d{3}$")

def _without_blender_duplicate_suffix(name):
    match = _BLENDER_DUP_SUFFIX_RE.match(name)
    return match.group(1) if match else name

def _is_ucx_collision_object(obj):
    return (
        obj is not None
        and obj.type == 'MESH'
        and _UCX_PREFIX_RE.match(_without_blender_duplicate_suffix(obj.name)) is not None
    )

def _get_ucx_body_names(collision_obj):
    """Return possible body names after UCX_ prefix, with and without numeric UCX suffix."""
    clean_name = _without_blender_duplicate_suffix(collision_obj.name)
    match = _UCX_PREFIX_RE.match(clean_name)
    if not match:
        return []

    body = match.group(1)
    bodies = [body]

    suffix_match = _UCX_SUFFIX_RE.match(body)
    if suffix_match:
        bodies.append(suffix_match.group(1))

    # Preserve order while removing duplicates.
    unique = []
    for item in bodies:
        if item not in unique:
            unique.append(item)
    return unique

def _find_ucx_target_mesh(collision_obj, render_mesh_objects):
    """Find render mesh for UCX_<MeshName> or UCX_<MeshName>_NN.

    Matching is intentionally based on the longest render mesh name first.
    This avoids wrong matches when names share prefixes, e.g. door and door_handle.
    """
    bodies = _get_ucx_body_names(collision_obj)
    if not bodies:
        return None

    candidates = []
    for obj in render_mesh_objects:
        if obj is None or obj.name not in bpy.data.objects:
            continue
        names = [obj.name]
        if obj.data and obj.data.name not in names:
            names.append(obj.data.name)
        for n in names:
            candidates.append((n, obj))

    # Exact, case-sensitive pass.
    for body in bodies:
        for name, obj in sorted(candidates, key=lambda item: len(item[0]), reverse=True):
            if body == name:
                return obj

    # Prefix + numeric suffix pass: UCX_body_01 -> body.
    clean_collision_name = _without_blender_duplicate_suffix(collision_obj.name)
    raw_body = _UCX_PREFIX_RE.match(clean_collision_name).group(1)
    for name, obj in sorted(candidates, key=lambda item: len(item[0]), reverse=True):
        if raw_body == name or re.match(r"^" + re.escape(name) + r"_\d{1,3}$", raw_body):
            return obj

    # Case-insensitive fallback.
    lower_bodies = [b.lower() for b in bodies]
    for body in lower_bodies:
        for name, obj in sorted(candidates, key=lambda item: len(item[0]), reverse=True):
            if body == name.lower():
                return obj

    raw_body_lower = raw_body.lower()
    for name, obj in sorted(candidates, key=lambda item: len(item[0]), reverse=True):
        lname = name.lower()
        if raw_body_lower == lname or re.match(r"^" + re.escape(lname) + r"_\d{1,3}$", raw_body_lower):
            return obj

    return None

def _set_origin_like_target_keep_world_geometry(obj, target_obj, copy_rotation_scale=True):
    """Move obj origin/pivot to target origin without moving obj geometry in world space.

    Important for articulated assets:
    - UCX collisions should keep the same parent-space logic as the mesh they belong to.
    - If the render mesh is positioned by a parent EMPTY and has zeroed local transforms,
      the UCX object receives the same parent, matrix_parent_inverse and local/basis matrix.
    - Collision vertices are compensated so the collision shape does not visually move.
    """
    if obj is None or target_obj is None:
        return False
    if obj.type != 'MESH' or target_obj.type != 'MESH':
        return False

    if obj.data and obj.data.users > 1:
        print(f"UCX Pivot Align: skipped shared mesh data for '{obj.name}' to preserve instancing.")
        return False

    old_world = obj.matrix_world.copy()

    child_worlds = [(ch, ch.matrix_world.copy()) for ch in obj.children]

    if copy_rotation_scale:
        # Copy the target object's hierarchy relationship, not just its world matrix.
        # This preserves setups like: DoorPivot_EMPTY transforms the door, while the
        # door mesh itself has zero local transforms and hinge/pivot logic lives in the parent.
        obj.parent = target_obj.parent
        obj.matrix_parent_inverse = target_obj.matrix_parent_inverse.copy()
        obj.matrix_basis = target_obj.matrix_basis.copy()
        new_world = obj.matrix_world.copy()
    else:
        # Fallback mode: keep the UCX object's current orientation/scale, but move the origin
        # to the target origin in world space. This mode is kept for safety, although the UI
        # currently uses the hierarchy-preserving copy_rotation_scale=True path.
        new_world = obj.matrix_world.copy()
        new_world.translation = target_obj.matrix_world.translation.copy()
        obj.matrix_world = new_world
        new_world = obj.matrix_world.copy()

    local_correction = new_world.inverted() @ old_world
    obj.data.transform(local_correction)
    obj.data.update()

    # Keep possible children visually unchanged. UCX objects normally have no children,
    # but this makes the operation safer in mixed production scenes.
    for ch, ch_world in child_worlds:
        if ch and ch.name in bpy.data.objects:
            ch.matrix_world = ch_world

    return True

def align_ucx_collision_pivots(context, roots, traverse_children=True, universe_objects=None):
    """Align UCX collision object pivots to their matching render mesh pivots.

    Supported names:
      UCX_<MeshName>
      UCX_<MeshName>_01 ... UCX_<MeshName>_999

    Geometry stays in the same world position; only the object origin/matrix is changed.
    """
    if traverse_children:
        universe = _collect_subtree(roots)
    else:
        universe = list(universe_objects) if universe_objects is not None else list(roots)

    universe = [o for o in universe if o and o.name in bpy.data.objects and o.type == 'MESH']
    collision_objects = [o for o in universe if _is_ucx_collision_object(o)]
    render_mesh_objects = [o for o in universe if not _is_ucx_collision_object(o)]

    aligned_count = 0
    skipped_count = 0

    for col_obj in collision_objects:
        if col_obj.name not in bpy.data.objects:
            skipped_count += 1
            continue
        col_obj = bpy.data.objects[col_obj.name]

        target = _find_ucx_target_mesh(col_obj, render_mesh_objects)
        if target is None:
            skipped_count += 1
            print(f"UCX Pivot Align: target mesh not found for '{col_obj.name}'")
            continue

        if _set_origin_like_target_keep_world_geometry(col_obj, target, copy_rotation_scale=True):
            aligned_count += 1
            print(f"UCX Pivot Align: '{col_obj.name}' -> '{target.name}' (parent-space)")
        else:
            skipped_count += 1

    return aligned_count, skipped_count

class OBJECT_OT_AlignUCXCollisionPivots(bpy.types.Operator):
    bl_idname = "object.align_ucx_collision_pivots"
    bl_label = "Align UCX Collision Pivots"
    bl_description = "Move UCX collision pivots to matching render mesh pivots and preserve target parent-space hierarchy."
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        sel_mode, selection = _get_selection_roots(context)

        if sel_mode == "objects":
            roots = list(selection)
            if not roots:
                self.report({'WARNING'}, "No selected objects.")
                return {'FINISHED'}
            aligned, skipped = align_ucx_collision_pivots(context, roots=roots, traverse_children=True)
            self.report({'INFO'}, f"Aligned UCX pivots: {aligned}. Skipped: {skipped}.")
        elif sel_mode == "collections":
            aligned_sum = 0
            skipped_sum = 0
            for col in selection:
                all_objs = list(col.all_objects)
                aligned, skipped = align_ucx_collision_pivots(
                    context,
                    roots=all_objs,
                    traverse_children=False,
                    universe_objects=all_objs
                )
                aligned_sum += aligned
                skipped_sum += skipped
            self.report({'INFO'}, f"Aligned UCX pivots: {aligned_sum}. Skipped: {skipped_sum}.")
        else:
            self.report({'WARNING'}, "Select objects or collections first.")

        return {'FINISHED'}


# --- Transform shell baking ---------------------------------------------------

def _safe_object_name(base_name):
    name = str(base_name).strip() if base_name else "Object"
    name = re.sub(r"[^0-9A-Za-zА-Яа-я_\.\-]+", "_", name)
    name = name.strip("_")
    return name or "Object"

def _make_identity_matrix():
    return Matrix.Identity(4)

def bake_selected_objects_to_transform_shell(context, selected_objects=None, target_obj=None, shell_suffix="_ROOT"):
    """Create an EMPTY shell from the active target transform and zero selected mesh objects under it.

    Workflow:
    - User selects render mesh + UCX collisions.
    - Active object is the target with correct pivot/orientation/parent-space transform.
    - New EMPTY receives the target parent, matrix_parent_inverse and local matrix_basis.
    - Selected mesh objects become children of this EMPTY with local identity transform.
    - Mesh vertex data is compensated so world geometry does not move.

    Result for export:
    - EMPTY carries the placement/rotation/pivot.
    - Meshes/collisions stay visually unchanged but have zeroed local transforms under the shell.
    """
    if selected_objects is None:
        selected_objects = list(getattr(context, "selected_objects", []) or [])
    else:
        selected_objects = list(selected_objects)

    if target_obj is None:
        target_obj = context.view_layer.objects.active

    if target_obj is None or target_obj.name not in bpy.data.objects:
        raise RuntimeError("No active target object. Select meshes/collisions and make the target object active last.")

    target_obj = bpy.data.objects[target_obj.name]
    if target_obj not in selected_objects:
        selected_objects.append(target_obj)

    bake_objects = []
    skipped_objects = []
    for obj in selected_objects:
        if obj is None or obj.name not in bpy.data.objects:
            continue
        obj = bpy.data.objects[obj.name]
        if obj.type == 'MESH':
            bake_objects.append(obj)
        else:
            skipped_objects.append(obj)

    # Preserve order and remove duplicates.
    unique_bake = []
    seen = set()
    for obj in bake_objects:
        if obj.name not in seen:
            unique_bake.append(obj)
            seen.add(obj.name)
    bake_objects = unique_bake

    if not bake_objects:
        raise RuntimeError("No selected mesh objects to bake under transform shell.")
    if target_obj.type != 'MESH':
        raise RuntimeError("Active target must be a MESH object for this tool.")

    shell_name = _unique_name(_safe_object_name(target_obj.name) + shell_suffix, set(bpy.data.objects.keys()))
    shell = bpy.data.objects.new(shell_name, None)
    shell.empty_display_type = 'PLAIN_AXES'
    shell.empty_display_size = max(0.1, min(2.0, max(target_obj.dimensions) * 0.25 if target_obj.dimensions else 0.5))

    target_collections = list(getattr(target_obj, "users_collection", []))
    _link_object_to_collections(shell, target_collections, context.scene.collection)

    # Copy target parent-space relationship to the shell, not just the world matrix.
    shell.parent = target_obj.parent
    shell.matrix_parent_inverse = target_obj.matrix_parent_inverse.copy()
    shell.matrix_basis = target_obj.matrix_basis.copy()
    shell_world = shell.matrix_world.copy()

    baked_count = 0
    for obj in bake_objects:
        if obj.name not in bpy.data.objects:
            continue
        obj = bpy.data.objects[obj.name]
        old_world = obj.matrix_world.copy()
        child_worlds = [(ch, ch.matrix_world.copy()) for ch in obj.children if ch != shell]

        if obj.data and obj.data.users > 1:
            skipped_objects.append(obj)
            print(f"Transform Shell: skipped shared mesh data for '{obj.name}' to preserve instancing.")
            continue

        obj.parent = shell
        obj.matrix_parent_inverse = _make_identity_matrix()
        obj.matrix_basis = _make_identity_matrix()

        new_world = obj.matrix_world.copy()
        local_correction = new_world.inverted() @ old_world
        obj.data.transform(local_correction)
        obj.data.update()

        for ch, ch_world in child_worlds:
            if ch and ch.name in bpy.data.objects:
                ch.matrix_world = ch_world

        baked_count += 1
        print(f"Transform Shell: '{obj.name}' baked under '{shell.name}'")

    # Select the new hierarchy for immediate inspection.
    try:
        bpy.ops.object.mode_set(mode='OBJECT')
    except Exception:
        pass
    bpy.ops.object.select_all(action='DESELECT')
    shell.select_set(True)
    for obj in bake_objects:
        if obj.name in bpy.data.objects:
            bpy.data.objects[obj.name].select_set(True)
    context.view_layer.objects.active = shell

    print(f"Transform Shell: created '{shell.name}' from target '{target_obj.name}'. Baked meshes: {baked_count}. Skipped non-mesh: {len(skipped_objects)}")
    return shell, baked_count, len(skipped_objects)

class OBJECT_OT_CreateTransformShell(bpy.types.Operator):
    bl_idname = "object.create_transform_shell"
    bl_label = "Create Transform Shell"
    bl_description = "Create an Empty from the active target transform and bake selected mesh/UCX objects to zero local transforms under it."
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        selected = list(getattr(context, "selected_objects", []) or [])
        active = context.view_layer.objects.active

        if not selected or active is None:
            self.report({'WARNING'}, "Select mesh/collision objects and make the target object active last.")
            return {'FINISHED'}

        try:
            shell, baked, skipped = bake_selected_objects_to_transform_shell(
                context,
                selected_objects=selected,
                target_obj=active,
                shell_suffix="_ROOT"
            )
        except Exception as ex:
            self.report({'ERROR'}, f"Create Transform Shell failed: {ex}")
            return {'CANCELLED'}

        self.report({'INFO'}, f"Created shell '{shell.name}'. Baked meshes: {baked}. Skipped non-mesh: {skipped}.")
        return {'FINISHED'}

def _collect_parent_empty_bake_scope(context):
    sel_mode, selection = _get_selection_roots(context)
    if sel_mode == "objects":
        return _collect_subtree(list(selection))

    if sel_mode == "collections":
        objects = []
        seen = set()
        for collection in selection:
            for obj in collection.all_objects:
                if obj in seen:
                    continue
                seen.add(obj)
                objects.append(obj)
        return objects

    return []

def _clear_object_delta_transforms(obj):
    obj.delta_location = (0.0, 0.0, 0.0)
    obj.delta_rotation_euler = (0.0, 0.0, 0.0)
    obj.delta_rotation_quaternion = (1.0, 0.0, 0.0, 0.0)
    obj.delta_scale = (1.0, 1.0, 1.0)

def _make_zero_location_local_matrix(parent_world, old_world):
    local_matrix = parent_world.inverted_safe() @ old_world
    local_matrix.translation = (0.0, 0.0, 0.0)
    return local_matrix

def _matrix_key(matrix, digits=6):
    return tuple(round(float(value), digits) for row in matrix for value in row)

def _mesh_data_users(mesh_data):
    return [
        obj for obj in bpy.data.objects
        if obj and obj.type == 'MESH' and obj.data is mesh_data
    ]

def _build_zero_parent_empty_plan(obj):
    if obj is None or obj.name not in bpy.data.objects:
        return None
    obj = bpy.data.objects[obj.name]
    if obj.type != 'MESH' or obj.parent is None or obj.parent.type != 'EMPTY':
        return None

    parent = obj.parent
    old_world = obj.matrix_world.copy()
    parent_world = parent.matrix_world.copy()
    new_local = _make_zero_location_local_matrix(parent_world, old_world)
    new_world = parent_world @ new_local
    local_correction = new_world.inverted_safe() @ old_world
    return {
        "obj": obj,
        "parent": parent,
        "mesh_data": obj.data,
        "new_local": new_local,
        "local_correction": local_correction,
        "child_worlds": [(ch, ch.matrix_world.copy()) for ch in obj.children],
    }

def _apply_zero_parent_empty_object_transform(plan):
    obj = plan["obj"]
    _clear_object_delta_transforms(obj)
    obj.matrix_parent_inverse = _make_identity_matrix()
    obj.matrix_basis = plan["new_local"]

    for child, child_world in plan["child_worlds"]:
        if child and child.name in bpy.data.objects:
            child.matrix_world = child_world

def zero_mesh_transforms_to_parent_empty(context, objects=None):
    """Zero child mesh local location under their current EMPTY parents.

    The mesh stays visually unchanged, but its object origin moves to the parent
    EMPTY transform and its local location becomes zero. Local rotation/scale are
    preserved to match existing ZIL-style door hierarchies.
    """
    if objects is None:
        objects = _collect_parent_empty_bake_scope(context)

    baked_count = 0
    skipped_count = 0
    seen = set()
    mesh_groups = {}

    for obj in objects:
        if obj is None or obj in seen or obj.name not in bpy.data.objects:
            continue
        seen.add(obj)
        obj = bpy.data.objects[obj.name]

        if obj.type != 'MESH' or obj.parent is None or obj.parent.type != 'EMPTY':
            skipped_count += 1
            continue

        mesh_groups.setdefault(obj.data, []).append(obj)

    for mesh_data, selected_users in mesh_groups.items():
        if mesh_data is None:
            skipped_count += len(selected_users)
            continue

        all_users = _mesh_data_users(mesh_data)
        if len(all_users) <= 1:
            plan = _build_zero_parent_empty_plan(selected_users[0])
            if plan is None:
                skipped_count += 1
                continue
            mesh_data.transform(plan["local_correction"])
            mesh_data.update()
            _apply_zero_parent_empty_object_transform(plan)
            baked_count += 1
            print(f"Zero Mesh To Parent Empty: '{plan['obj'].name}' baked under '{plan['parent'].name}'")
            continue

        # Shared mesh data must be processed as a whole. Transforming only one
        # selected object would move every linked user of the same mesh data.
        shared_plans = []
        incompatible_users = []
        for user in all_users:
            plan = _build_zero_parent_empty_plan(user)
            if plan is None:
                incompatible_users.append(user)
            else:
                shared_plans.append(plan)

        correction_keys = {_matrix_key(plan["local_correction"]) for plan in shared_plans}
        if incompatible_users or len(correction_keys) != 1:
            skipped_count += len(selected_users)
            print(
                f"Zero Mesh To Parent Empty: skipped shared mesh data '{mesh_data.name}' "
                f"({len(all_users)} user(s)) to preserve instancing."
            )
            continue

        local_correction = shared_plans[0]["local_correction"]
        mesh_data.transform(local_correction)
        mesh_data.update()

        for plan in shared_plans:
            _apply_zero_parent_empty_object_transform(plan)
            baked_count += 1
            print(f"Zero Mesh To Parent Empty: '{plan['obj'].name}' baked under '{plan['parent'].name}'")

    return baked_count, skipped_count

class OBJECT_OT_ZeroMeshTransformsToParentEmpty(bpy.types.Operator):
    bl_idname = "object.zero_mesh_transforms_to_parent_empty"
    bl_label = "Zero Mesh To Parent Empty"
    bl_description = "Bake selected/collection child meshes so they keep visual position under their current Empty parent but have local identity transforms."
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        try:
            baked, skipped = zero_mesh_transforms_to_parent_empty(context)
        except Exception as ex:
            self.report({'ERROR'}, f"Zero Mesh To Parent Empty failed: {ex}")
            return {'CANCELLED'}

        if baked == 0:
            self.report({'WARNING'}, "No child mesh with EMPTY parent found in selection.")
        else:
            self.report({'INFO'}, f"Zeroed mesh transforms: {baked}. Skipped: {skipped}.")
        return {'FINISHED'}

def _is_decal_shader_material(mat, export_cache=None):
    if mat is None:
        return False
    try:
        info = get_material_properties_cached(mat, export_cache)
        shader_class = info.get("shader_class")
    except Exception:
        shader_class = None
    if not shader_class:
        return False
    return str(shader_class).lower().endswith("_decal")

def separate_decal_faces_bottom_up(context, roots, traverse_children=True, universe_objects=None, export_cache=None):
    if traverse_children:
        universe = _collect_subtree(roots)
    else:
        universe = list(universe_objects) if universe_objects is not None else list(roots)

    targets = [o for o in universe if o and o.name in bpy.data.objects and o.type == 'MESH']
    created_total = 0

    prev_mode = getattr(context, "mode", "OBJECT")
    prev_active = context.view_layer.objects.active
    prev_selected = list(getattr(context, "selected_objects", []) or [])

    override = _get_view3d_override(context)
    if override is None:
        raise RuntimeError("No VIEW_3D area found. Open a 3D Viewport and try again.")

    with bpy.context.temp_override(**override):
        try:
            try:
                bpy.ops.object.mode_set(mode='OBJECT')
            except Exception:
                pass

            for obj in targets:
                if obj is None or obj.name not in bpy.data.objects:
                    continue
                obj = bpy.data.objects[obj.name]
                if obj.type != 'MESH':
                    continue

                decal_slots = []
                for i, slot in enumerate(obj.material_slots):
                    mat = slot.material
                    if _is_decal_shader_material(mat, export_cache):
                        decal_slots.append((i, mat))

                if not decal_slots:
                    continue

                # Split decals only on mixed meshes: ordinary geometry + decal faces.
                # If the mesh itself consists only of decal-assigned faces, leave it untouched.
                decal_slot_indices = {idx for idx, _mat in decal_slots}
                used_face_indices = {poly.material_index for poly in obj.data.polygons}
                has_decal_faces = any(idx in decal_slot_indices for idx in used_face_indices)
                has_non_decal_faces = any(idx not in decal_slot_indices for idx in used_face_indices)

                if not (has_decal_faces and has_non_decal_faces):
                    continue

                created_for_obj = []
                bpy.ops.object.select_all(action='DESELECT')
                obj.select_set(True)
                context.view_layer.objects.active = obj

                for mat_index, mat in decal_slots:
                    if obj.name not in bpy.data.objects:
                        break

                    bpy.ops.object.mode_set(mode='OBJECT')
                    bpy.ops.object.select_all(action='DESELECT')
                    obj.select_set(True)
                    context.view_layer.objects.active = obj

                    bpy.ops.object.mode_set(mode='EDIT')
                    bpy.ops.mesh.select_mode(type='FACE')
                    bpy.ops.mesh.select_all(action='DESELECT')

                    obj.active_material_index = mat_index
                    bpy.ops.object.material_slot_select()

                    bm = bmesh.from_edit_mesh(obj.data)
                    has_selected_faces = any(f.select for f in bm.faces)
                    if not has_selected_faces:
                        bpy.ops.object.mode_set(mode='OBJECT')
                        continue

                    bpy.ops.mesh.separate(type='SELECTED')
                    bpy.ops.object.mode_set(mode='OBJECT')

                    new_objs = [o for o in context.selected_objects if o != obj and o.type == 'MESH']
                    if not new_objs:
                        bpy.ops.object.select_all(action='DESELECT')
                        obj.select_set(True)
                        context.view_layer.objects.active = obj
                        continue

                    new_obj = new_objs[0]
                    parent = obj.parent
                    world = new_obj.matrix_world.copy()
                    new_obj.parent = parent
                    new_obj.matrix_world = world

                    existing = set(bpy.data.objects.keys())
                    safe_mat = mat.name.replace(" ", "_")
                    base_name = f"{obj.name}__{safe_mat}"
                    new_obj.name = _unique_name(base_name, existing)

                    created_for_obj.append(new_obj)
                    created_total += 1

                    bpy.ops.object.select_all(action='DESELECT')
                    obj.select_set(True)
                    context.view_layer.objects.active = obj

                if obj.name in bpy.data.objects:
                    bpy.ops.object.mode_set(mode='OBJECT')
                    bpy.ops.object.select_all(action='DESELECT')
                    obj.select_set(True)
                    context.view_layer.objects.active = obj
                    try:
                        bpy.ops.object.material_slot_remove_unused()
                    except Exception:
                        pass

                for n in created_for_obj:
                    if not n or n.name not in bpy.data.objects:
                        continue
                    bpy.ops.object.mode_set(mode='OBJECT')
                    bpy.ops.object.select_all(action='DESELECT')
                    n.select_set(True)
                    context.view_layer.objects.active = n
                    try:
                        bpy.ops.object.material_slot_remove_unused()
                    except Exception:
                        pass

        finally:
            try:
                bpy.ops.object.mode_set(mode='OBJECT')
            except Exception:
                pass
            bpy.ops.object.select_all(action='DESELECT')
            for o in prev_selected:
                if o and o.name in bpy.data.objects:
                    bpy.data.objects[o.name].select_set(True)
            if prev_active and prev_active.name in bpy.data.objects:
                context.view_layer.objects.active = bpy.data.objects[prev_active.name]
            try:
                if str(prev_mode).startswith("EDIT") and context.view_layer.objects.active:
                    bpy.ops.object.mode_set(mode='EDIT')
                else:
                    bpy.ops.object.mode_set(mode='OBJECT')
            except Exception:
                pass

    return created_total

class OBJECT_OT_SeparateDecalFaces(bpy.types.Operator):
    bl_idname = "object.separate_decal_faces"
    bl_label = "Split Decal Faces (by shader_class)"
    bl_description = "Split faces whose material shader_class ends with '*_decal'."
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        sel_mode, selection = _get_selection_roots(context)

        if sel_mode == "objects":
            roots = list(selection)
            if roots:
                created = separate_decal_faces_bottom_up(context, roots=roots, traverse_children=True)
                self.report({'INFO'}, f"Split decal faces: created {created} decal mesh object(s).")
            else:
                self.report({'WARNING'}, "No selected objects.")
        elif sel_mode == "collections":
            created_sum = 0
            for col in selection:
                all_objs = list(col.all_objects)
                created_sum += separate_decal_faces_bottom_up(context, roots=all_objs, traverse_children=False, universe_objects=all_objs)
            self.report({'INFO'}, f"Split decal faces: created {created_sum} decal mesh object(s).")
        else:
            self.report({'WARNING'}, "Select objects or collections first.")

        return {'FINISHED'}

class OBJECT_OT_DeduplicateMaterials(bpy.types.Operator):
    bl_idname = "object.deduplicate_materials"
    bl_label = "Clean Duplicate Materials"
    bl_description = "Globally replace duplicate materials and purge unused materials from the file"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        # 1. Проходимся по ВСЕМ мешам во всём проекте
        all_meshes = [obj for obj in bpy.data.objects if obj.type == 'MESH']
        
        total_fixed = 0
        for obj in all_meshes:
            total_fixed += deduplicate_material_slots(obj)
            
        # 2. Очистка файлов от неиспользуемых материалов (у которых 0 пользователей)
        purged_count = 0
        for mat in list(bpy.data.materials):
            if mat.users == 0:
                bpy.data.materials.remove(mat)
                purged_count += 1
        
        self.report({'INFO'}, f"Успех! Исправлено {total_fixed} слотов. Удалено {purged_count} неиспользуемых материалов.")
        return {'FINISHED'}


class DagUnrealPanel(bpy.types.Panel):
    bl_label = "Dag to Unreal"
    bl_idname = "OBJECT_PT_dag_to_unreal"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'DagUnreal'

    def draw(self, context):
        layout = self.layout
        scene = context.scene

        layout.prop(scene, "dest_folder", text="Folder")
        
        # Переключение режима экспорта в интерфейс
        layout.label(text="Export Mode:")
        layout.prop(scene, "export_mode", expand=True)
        
        layout.separator()
        layout.operator("export.asset_operator", text="Export Asset")
        layout.operator("export.texture_maps_operator", text="Export Texture Maps")
        layout.operator("object.make_collection_instances_real", text="Make Instances Real")
        
        # --- Инструменты подготовки ---
        layout.separator()
        layout.label(text="Preparation Tools:")
        layout.operator("object.deduplicate_materials", text="Clean Duplicate Mats", icon='MATERIAL')
        
        layout.prop(scene, "collapse_empty_mesh_groups", text="Collapse Empty→Mesh")
        layout.prop(scene, "split_decal_faces", text="Split Decal Faces")
        layout.prop(scene, "align_ucx_collision_pivots", text="Align UCX Pivots")
        layout.operator("object.separate_decal_faces", text="Split Decal Faces Now")
        layout.operator("object.align_ucx_collision_pivots", text="Align UCX Pivots Now")
        layout.operator("object.create_transform_shell", text="Create Transform Shell")
        layout.operator("object.zero_mesh_transforms_to_parent_empty", text="Zero Mesh→Parent Empty")
        layout.operator("object.collapse_empty_mesh_groups", text="Collapse Empty→Mesh Now")

class ExportAssetOperator(bpy.types.Operator):
    bl_idname = "export.asset_operator"
    bl_label = "Export Asset"

    def execute(self, context):
        dest_folder = convert_to_absolute_path(context.scene.dest_folder)
        copied_textures = set()
        export_cache = make_export_cache()
        if not os.path.exists(dest_folder):
            os.makedirs(dest_folder)
        area = next(area for area in bpy.context.window.screen.areas if area.type == 'OUTLINER')
        with bpy.context.temp_override(window=bpy.context.window, area=area, region=next(region for region in area.regions if region.type == 'WINDOW'), screen=bpy.context.window.screen):
            list_materials_and_shader_groups_in_selected_collections(dest_folder, copied_textures, export_cache)
        self.report({'INFO'}, f"Assets exported successfully to {dest_folder}!")
        return {'FINISHED'}

class ExportTextureMapsOperator(bpy.types.Operator):
    bl_idname = "export.texture_maps_operator"
    bl_label = "Export Texture Maps"
    bl_description = "Copy texture maps from selected objects or collections to Destination Folder, preserving EnlistedCDK path structure"

    def execute(self, context):
        dest_folder = convert_to_absolute_path(context.scene.dest_folder)
        copied_textures = set()
        export_cache = make_export_cache()
        if not os.path.exists(dest_folder):
            os.makedirs(dest_folder)

        area = next(area for area in bpy.context.window.screen.areas if area.type == 'OUTLINER')
        with bpy.context.temp_override(window=bpy.context.window, area=area, region=next(region for region in area.regions if region.type == 'WINDOW'), screen=bpy.context.window.screen):
            object_count, material_count, texture_count = export_texture_maps_for_current_selection(dest_folder, copied_textures, export_cache)

        self.report({'INFO'}, f"Texture maps exported: {texture_count} file(s), {material_count} material(s), {object_count} object(s).")
        return {'FINISHED'}

class OBJECT_OT_MakeCollectionInstancesReal(bpy.types.Operator):
    bl_idname = "object.make_collection_instances_real"
    bl_label = "Make Instances Real"
    bl_description = "Convert all selected collection instances to real objects"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        area = next(area for area in context.window.screen.areas if area.type == 'OUTLINER')
        
        if not area:
            self.report({'ERROR'}, "No Outliner area found")
            return {'CANCELLED'}
        
        new_context = context.copy()
        new_context['area'] = area
        new_context['region'] = next(region for region in area.regions if region.type == 'WINDOW')

        with bpy.context.temp_override(window=context.window, area=area, region=new_context['region'], screen=context.window.screen):
            make_collection_instances_real(context)
        
        self.report({'INFO'}, "Collection instances converted to real objects.")
        return {'FINISHED'}

def register():
    bpy.utils.register_class(DagUnrealPanel)
    bpy.utils.register_class(ExportAssetOperator)
    bpy.utils.register_class(ExportTextureMapsOperator)
    bpy.utils.register_class(OBJECT_OT_MakeCollectionInstancesReal)
    bpy.utils.register_class(OBJECT_OT_CollapseEmptyMeshGroups)
    bpy.utils.register_class(OBJECT_OT_SeparateDecalFaces)
    bpy.utils.register_class(OBJECT_OT_AlignUCXCollisionPivots)
    bpy.utils.register_class(OBJECT_OT_CreateTransformShell)
    bpy.utils.register_class(OBJECT_OT_ZeroMeshTransformsToParentEmpty)
    bpy.utils.register_class(OBJECT_OT_DeduplicateMaterials)
    
    # Регистрация новой настройки режима экспорта
    bpy.types.Scene.export_mode = bpy.props.EnumProperty(
        name="Export Mode",
        items=[
            ('STATIC', "Static Mesh", "Export geometry as static layout"),
            ('SKELETAL', "Skeletal Mesh", "Export with Armatures / Bone chains included")
        ],
        default='STATIC'
    )
    
    bpy.types.Scene.collapse_empty_mesh_groups = bpy.props.BoolProperty(
        name="Collapse Empty→Mesh",
        description="Before export: replace empties that have direct mesh children by a merged mesh (bottom-up)",
        default=True,
    )
    bpy.types.Scene.split_decal_faces = bpy.props.BoolProperty(
        name="Split Decal Faces",
        description="Before export: split faces whose material shader_class ends with '*_decal'",
        default=False,
    )
    bpy.types.Scene.align_ucx_collision_pivots = bpy.props.BoolProperty(
        name="Align UCX Collision Pivots",
        description="Before export: align UCX collision pivots to matching render meshes and preserve parent-space hierarchy",
        default=False,
    )
    bpy.types.Scene.dest_folder = bpy.props.StringProperty(name="Destination Folder", subtype='DIR_PATH')

def unregister():
    bpy.utils.unregister_class(DagUnrealPanel)
    bpy.utils.unregister_class(ExportAssetOperator)
    bpy.utils.unregister_class(ExportTextureMapsOperator)
    bpy.utils.unregister_class(OBJECT_OT_MakeCollectionInstancesReal)
    bpy.utils.unregister_class(OBJECT_OT_CollapseEmptyMeshGroups)
    bpy.utils.unregister_class(OBJECT_OT_SeparateDecalFaces)
    bpy.utils.unregister_class(OBJECT_OT_AlignUCXCollisionPivots)
    bpy.utils.unregister_class(OBJECT_OT_CreateTransformShell)
    bpy.utils.unregister_class(OBJECT_OT_ZeroMeshTransformsToParentEmpty)
    bpy.utils.unregister_class(OBJECT_OT_DeduplicateMaterials)
    
    del bpy.types.Scene.dest_folder
    del bpy.types.Scene.collapse_empty_mesh_groups
    del bpy.types.Scene.split_decal_faces
    del bpy.types.Scene.align_ucx_collision_pivots
    del bpy.types.Scene.export_mode 

if __name__ == "__main__":
    register()
