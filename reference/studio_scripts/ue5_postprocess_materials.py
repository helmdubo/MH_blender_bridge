import unreal
import json
import os


# ============================================================
# SETTINGS
# ============================================================

MASTER_MATERIAL_FOLDER = "/Game/MimirHead/MasterMaterials"
COMMON_TEXTURE_FOLDER = "/Game/ImportedTextures"
MATERIALS_SUBFOLDER_NAME = "materials"

# True: rebuild/update Material Instance from metadata ONLY when it is actually needed:
# - MI is newly created
# - mesh slot currently contains wrong material / master material / empty slot
# - existing MI has wrong parent master material
#
# Correct existing MI in a correct mesh slot is NOT touched: textures/scalars/vectors are not rewritten.
REBUILD_WRONG_MATERIAL_INSTANCES_FROM_METADATA = True

# Component/Actor scales are intentionally left unchanged.
# The script does not normalize or reset transforms.

# True: for StaticMeshComponent whose component name OR assigned StaticMesh asset name
# ends with _decal / _decals, disable shadow casting on the component.
# Works for Blueprint components and plain Actor components.
DISABLE_SHADOWS_FOR_DECAL_STATIC_MESH_COMPONENTS = True
DECAL_STATIC_MESH_NAME_SUFFIXES = ("_decal", "_decals")

# True: for StaticMesh asset whose asset name ends with _decal / _decals,
# disable section-level shadow casting globally on the StaticMesh asset itself.
# This is separate from component Cast Shadow and is applied to all LOD sections.
DISABLE_SHADOWS_FOR_DECAL_STATIC_MESH_ASSETS = True
STATIC_MESH_SECTION_SCAN_FALLBACK_LIMIT = 64

# Because UE Python does not expose a reliable public getter for section Cast Shadow in all versions,
# the script writes a metadata marker after it has applied asset-level decal shadow settings once.
# This avoids dirtying/saving the same decal StaticMesh asset again on every repeated run.
# Set to False if you deliberately want to force-reapply section Cast Shadow = False.
SKIP_DECAL_STATIC_MESH_ASSET_SHADOWS_IF_MARKER_EXISTS = True
DECAL_STATIC_MESH_ASSET_SHADOWS_METADATA_TAG = "MimirHead_DecalStaticMeshAssetShadowsOff"

# True: remove plain SceneComponent objects whose name starts with UCX_.
# These are usually empty wrappers left after importing DCC collision objects.
# StaticMeshComponent/SkeletalMeshComponent are NOT deleted by this rule.
CLEAN_UCX_SCENE_COMPONENTS = True
UCX_SCENE_COMPONENT_PREFIXES = ("UCX_",)

# True: for every unique StaticMesh collected from the selected Blueprint/Actor,
# set Build Settings -> Max Lumen Mesh Cards to this value.
SET_MAX_LUMEN_MESH_CARDS_FOR_STATIC_MESHES = True
TARGET_MAX_LUMEN_MESH_CARDS = 32

# Metadata shader aliases.
# By default metadata shader names resolve 1:1 to UE master material names.
# Keep this dict empty unless a deliberate legacy remap is required.
SHADER_CLASS_ALIASES = {}

# Texture import performance mode.
# True: validate/fix suffix settings on already-existing textures too.
# False: only configure newly imported textures; existing textures are not loaded/saved during the global pass.
# This is much faster for repeated runs and avoids point-like re-saves of old texture assets.
POSTPROCESS_EXISTING_TEXTURES = False

# Dirty bulk texture import mode.
# Missing textures are imported in one AssetTools.import_asset_tasks(...) call with AssetImportTask.save = False.
# No TextureFactory suffix settings are applied during import.
# After import, the script reads only the Texture assets actually created by the import tasks,
# applies suffix properties to that array, then runs one batch save_loaded_assets(..., only_if_is_dirty=True).
DIRTY_BULK_IMPORT_TEXTURES_NO_SAVE = True

# Reduce per-texture log spam. Summary logs are still printed.
VERBOSE_TEXTURE_POST_LOGS = False


# ============================================================
# JSON / METADATA NORMALIZATION
# ============================================================

def decode_json_safely(value):
    try:
        decoded_value = json.loads(value)

        # Иногда JSON может быть записан как строка внутри строки
        if isinstance(decoded_value, str):
            decoded_value = json.loads(decoded_value)

        return decoded_value

    except json.JSONDecodeError as e:
        unreal.log_error(f"Failed to decode JSON: {value}. Error: {str(e)}")
        return None

    except Exception as e:
        unreal.log_error(f"Unexpected JSON decode error: {value}. Error: {str(e)}")
        return None


def get_first_existing_value(data, keys, default=None):
    if not isinstance(data, dict):
        return default

    for key in keys:
        if key in data and data.get(key) not in [None, ""]:
            return data.get(key)

    return default


def normalize_texture_dict(raw_textures):
    """
    Поддерживает оба частых формата:
    1) textures = {"tex0": "D:/path/texture.tga"}
    2) Textures = {"tex0": {"path": "D:/path/texture.tga", "name": "..."}}
    """
    normalized = {}

    if not isinstance(raw_textures, dict):
        return normalized

    for tex_key, tex_value in raw_textures.items():
        if not tex_value:
            continue

        if isinstance(tex_value, str):
            normalized[str(tex_key)] = tex_value

        elif isinstance(tex_value, dict):
            tex_path = get_first_existing_value(
                tex_value,
                ["path", "filepath", "file_path", "Path", "FilePath", "File Path"]
            )

            if tex_path:
                normalized[str(tex_key)] = tex_path

    return normalized


def normalize_optional_parameters(raw_parameters):
    """
    Оставляет scalar и vector4 параметры.
    Остальное не трогает, чтобы не сломать нестандартные metadata.
    """
    normalized = {}

    if not isinstance(raw_parameters, dict):
        return normalized

    for param_name, param_value in raw_parameters.items():
        if isinstance(param_value, (float, int)):
            normalized[str(param_name)] = float(param_value)

        elif isinstance(param_value, list) and len(param_value) == 4:
            normalized[str(param_name)] = [
                float(param_value[0]),
                float(param_value[1]),
                float(param_value[2]),
                float(param_value[3])
            ]

    return normalized


def normalize_shader_class(shader_class):
    shader_name = str(shader_class).strip() if shader_class is not None else "Default"

    if not shader_name:
        shader_name = "Default"

    return SHADER_CLASS_ALIASES.get(shader_name, shader_name)


def parse_material_metadata(metadata):
    """
    Возвращает унифицированные поля из metadata.
    Старый формат исходного скрипта:
      shader_class, textures, optional
    Возможный формат из Blender/FBX pipeline:
      Shader Group Name, Textures, Parameters
    """
    if not isinstance(metadata, dict):
        return None, {}, {}

    shader_class = get_first_existing_value(
        metadata,
        [
            "shader_class",
            "Shader Group Name",
            "shader_group",
            "Shader Group",
            "UE Material",
            "ue_material"
        ],
        "Default"
    )

    raw_textures = get_first_existing_value(
        metadata,
        ["textures", "Textures"],
        {}
    )

    raw_parameters = get_first_existing_value(
        metadata,
        ["optional", "parameters", "Parameters", "Optional"],
        {}
    )

    return normalize_shader_class(shader_class), normalize_texture_dict(raw_textures), normalize_optional_parameters(raw_parameters)


# ============================================================
# ASSET HELPERS
# ============================================================

def get_asset_path(asset):
    if not asset:
        return "None"

    try:
        return asset.get_path_name()
    except Exception:
        return str(asset)


def ensure_directory(path):
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        unreal.EditorAssetLibrary.make_directory(path)


def is_same_asset(asset_a, asset_b):
    if not asset_a or not asset_b:
        return False

    return get_asset_path(asset_a) == get_asset_path(asset_b)


# ============================================================
# TEXTURES
# ============================================================

# Runtime cache for texture assets resolved during the global bulk import/post-process pass.
# This avoids re-loading the same Texture asset repeatedly while assigning MI parameters.
GLOBAL_TEXTURE_ASSET_CACHE = {}


def normalize_texture_source_path(texture_path):
    if not texture_path:
        return ""

    return str(texture_path).replace("\\", "/").strip()


def get_texture_name_from_source_path(texture_path):
    """
    Keeps the original script's naming rule: basename split at the first dot.
    Example: D:/tex/body_tex_d.tga -> body_tex_d
    """
    texture_path = normalize_texture_source_path(texture_path)
    return os.path.basename(texture_path).split('.')[0]


def get_texture_asset_path_from_source_path(texture_path, destination_folder):
    texture_name = get_texture_name_from_source_path(texture_path)

    if not texture_name:
        return "", ""

    return f"{destination_folder}/{texture_name}", texture_name


def get_texture_suffix_settings(texture_name):
    """
    Preserves the existing suffix rules exactly.
    No semantic changes for _tex_n/_n, _tex_m/_m, _tex_d/_d.
    """
    if texture_name.endswith(("_tex_d", "_d")):
        return {
            "compression_settings": unreal.TextureCompressionSettings.TC_DEFAULT,
            "sRGB": True,
        }

    if texture_name.endswith(("_tex_n", "_n")):
        return {
            "compression_settings": unreal.TextureCompressionSettings.TC_BC7,
            "sRGB": False,
        }

    if texture_name.endswith(("_tex_m", "_m")):
        return {
            "compression_settings": unreal.TextureCompressionSettings.TC_DEFAULT,
            "sRGB": True,
        }

    return {}


def get_editor_property_with_fallback(asset, property_names):
    for property_name in property_names:
        try:
            return asset.get_editor_property(property_name)
        except Exception:
            pass

    raise RuntimeError(f"Не удалось прочитать editor property: {property_names}")


def set_editor_property_with_fallback(asset, property_names, value):
    last_error = None

    for property_name in property_names:
        try:
            asset.set_editor_property(property_name, value)
            return True
        except Exception as e:
            last_error = e

    raise RuntimeError(f"Не удалось установить editor property {property_names}: {str(last_error)}")


def update_texture_property_if_needed(texture_asset, property_names, target_value):
    """
    Returns True only when the property was actually changed.
    If reading the current value fails, tries to set the target value once.
    """
    try:
        current_value = get_editor_property_with_fallback(texture_asset, property_names)

        if current_value == target_value:
            return False

    except Exception:
        # If the property exists but reading failed for some UE-version-specific reason,
        # still try to set it. The setter below will report a useful error if impossible.
        pass

    set_editor_property_with_fallback(texture_asset, property_names, target_value)
    return True


def configure_texture_by_suffix_if_needed(texture_asset, texture_name):
    suffix_settings = get_texture_suffix_settings(texture_name)

    if not suffix_settings:
        return False

    changed = False

    try:
        if "compression_settings" in suffix_settings:
            if update_texture_property_if_needed(
                texture_asset,
                ["compression_settings"],
                suffix_settings["compression_settings"]
            ):
                changed = True

        if "sRGB" in suffix_settings:
            if update_texture_property_if_needed(
                texture_asset,
                ["sRGB", "srgb"],
                bool(suffix_settings["sRGB"])
            ):
                changed = True

        if changed and VERBOSE_TEXTURE_POST_LOGS:
            unreal.log(
                f"TEXTURE POST: обновлены настройки Texture '{texture_asset.get_name()}' "
                f"по suffix rule."
            )

    except Exception as e:
        unreal.log_warning(
            f"Не удалось обновить настройки текстуры '{texture_asset.get_path_name()}': {str(e)}"
        )

    return changed


def save_loaded_assets_batch_safely(assets, log_label="assets"):
    if not assets:
        return True

    unique_assets_by_path = {}

    for asset in assets:
        if not asset:
            continue

        unique_assets_by_path[get_asset_path(asset)] = asset

    unique_assets = list(unique_assets_by_path.values())

    if not unique_assets:
        return True

    try:
        unreal.EditorAssetLibrary.save_loaded_assets(unique_assets, True)
        unreal.log(f"BATCH SAVE: сохранено {len(unique_assets)} {log_label}.")
        return True

    except Exception as e:
        unreal.log_warning(
            f"save_loaded_assets для {len(unique_assets)} {log_label} не сработал, "
            f"использую fallback save_loaded_asset по одному. Ошибка: {str(e)}"
        )

        all_saved = True

        for asset in unique_assets:
            try:
                unreal.EditorAssetLibrary.save_loaded_asset(asset)
            except Exception as e_single:
                all_saved = False
                unreal.log_warning(
                    f"Не удалось сохранить asset '{get_asset_path(asset)}': {str(e_single)}"
                )

        return all_saved


def get_import_task_imported_object_paths(import_task):
    """
    Returns AssetImportTask.imported_object_paths after import.
    UE Python exposure differs slightly between versions, so several access paths are tried.
    """
    if not import_task:
        return []

    imported_paths = []

    try:
        imported_paths = import_task.get_editor_property("imported_object_paths")
    except Exception:
        try:
            imported_paths = import_task.imported_object_paths
        except Exception:
            imported_paths = []

    if not imported_paths:
        return []

    return [str(path_value) for path_value in imported_paths if path_value]


def get_import_task_result_assets(import_task):
    """
    Fallback/extra route: AssetImportTask.result can contain loaded imported objects.
    imported_object_paths is still preferred for diagnostics and robustness.
    """
    if not import_task:
        return []

    try:
        result_assets = import_task.get_editor_property("result")
    except Exception:
        try:
            result_assets = import_task.result
        except Exception:
            result_assets = []

    if not result_assets:
        return []

    return [asset for asset in result_assets if asset]


def normalize_imported_object_path_candidates(path_value):
    """
    Builds candidate paths for unreal.load_asset() from AssetImportTask.imported_object_paths.
    Depending on UE version/importer, paths may look like:
      /Game/ImportedTextures/foo.foo
      /Game/ImportedTextures/foo
      Texture2D'/Game/ImportedTextures/foo.foo'
    """
    if path_value is None:
        return []

    raw = str(path_value).strip()
    if not raw:
        return []

    candidates = []

    def add_candidate(candidate):
        candidate = str(candidate).strip()
        if candidate and candidate not in candidates:
            candidates.append(candidate)

    add_candidate(raw)

    if "'" in raw:
        try:
            inner = raw.split("'", 1)[1].rsplit("'", 1)[0]
            add_candidate(inner)
        except Exception:
            pass

    # Convert object path /Game/Folder/Asset.Asset -> package-style path /Game/Folder/Asset.
    for candidate in list(candidates):
        try:
            folder_part, object_part = candidate.rsplit("/", 1)
        except ValueError:
            continue

        if "." in object_part:
            asset_name = object_part.split(".", 1)[0]
            add_candidate(f"{folder_part}/{asset_name}")

    return candidates


def load_asset_from_imported_object_path(path_value):
    for candidate in normalize_imported_object_path_candidates(path_value):
        try:
            asset = unreal.load_asset(candidate)
        except Exception:
            asset = None

        if asset:
            return asset

    return None


def get_imported_texture_asset_from_task(import_task, expected_asset_path):
    """
    Resolves the Texture asset produced by one import task.
    Preferred order:
    1) AssetImportTask.imported_object_paths -> load_asset
    2) AssetImportTask.result loaded assets
    3) expected destination asset path fallback
    """
    for imported_path in get_import_task_imported_object_paths(import_task):
        texture_asset = load_asset_from_imported_object_path(imported_path)
        if texture_asset:
            return texture_asset

    for result_asset in get_import_task_result_assets(import_task):
        if result_asset:
            return result_asset

    try:
        return unreal.load_asset(expected_asset_path)
    except Exception:
        return None


def build_unique_texture_import_records(texture_paths, destination_folder):
    """
    Returns records deduplicated by destination asset path.
    If two different source files would produce the same UE asset path, the first one wins,
    because the original script also resolved textures only by basename in COMMON_TEXTURE_FOLDER.
    """
    records_by_asset_path = {}

    for raw_texture_path in texture_paths:
        texture_path = normalize_texture_source_path(raw_texture_path)

        if not texture_path:
            continue

        texture_asset_path, texture_name = get_texture_asset_path_from_source_path(
            texture_path,
            destination_folder
        )

        if not texture_asset_path:
            continue

        if texture_asset_path in records_by_asset_path:
            existing_source_path = records_by_asset_path[texture_asset_path]["source_path"]

            if existing_source_path != texture_path:
                unreal.log_warning(
                    f"TEXTURE BULK: разные source paths дают один asset path '{texture_asset_path}'. "
                    f"Использую первый: '{existing_source_path}', пропускаю: '{texture_path}'."
                )

            continue

        records_by_asset_path[texture_asset_path] = {
            "source_path": texture_path,
            "texture_name": texture_name,
            "asset_path": texture_asset_path,
        }

    return list(records_by_asset_path.values())


def import_textures(texture_paths, destination_folder):
    """
    Global dirty bulk texture import + post-import suffix edit + one batch save.

    Performance intent:
    - collect unique texture records by destination asset path;
    - filter out existing assets before import;
    - import all missing textures in one import_asset_tasks() call;
    - AssetImportTask.save = False, so no per-task save during import;
    - do NOT change TextureFactory/import properties during import;
    - resolve only assets actually produced by those import tasks via imported_object_paths/result;
    - apply suffix rules to that imported array;
    - save the imported/dirty textures through one save_loaded_assets(..., only_if_is_dirty=True) call.
    """
    ensure_directory(destination_folder)

    records = build_unique_texture_import_records(texture_paths, destination_folder)

    if not records:
        unreal.log("TEXTURE BULK: текстуры для импорта/post-process не найдены.")
        return []

    texture_import_tasks = []
    new_records_by_asset_path = {}
    existing_records = []

    for record in records:
        texture_asset_path = record["asset_path"]

        if unreal.EditorAssetLibrary.does_asset_exist(texture_asset_path):
            existing_records.append(record)
            continue

        texture_import_task = unreal.AssetImportTask()
        texture_import_task.filename = record["source_path"]
        texture_import_task.destination_path = destination_folder
        texture_import_task.automated = True
        texture_import_task.save = False

        # We already filtered existing destination assets above. Keeping replace_existing=False avoids
        # accidentally dirty-importing an existing asset if a race/stale path slips through.
        texture_import_task.replace_existing = False

        # Keep the old destination asset naming assumption deterministic.
        try:
            texture_import_task.destination_name = record["texture_name"]
        except Exception:
            pass

        texture_import_tasks.append(texture_import_task)
        new_records_by_asset_path[texture_asset_path] = record
        record["import_task"] = texture_import_task

    unreal.log(
        f"TEXTURE DIRTY BULK: всего уникальных texture assets: {len(records)}, "
        f"новых к импорту: {len(texture_import_tasks)}, уже существуют: {len(existing_records)}, "
        f"postprocess existing: {POSTPROCESS_EXISTING_TEXTURES}."
    )

    if texture_import_tasks:
        unreal.log(
            f"TEXTURE DIRTY BULK IMPORT: один import_asset_tasks batch без save: "
            f"{len(texture_import_tasks)} шт. -> {destination_folder}"
        )
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(texture_import_tasks)

    processed_textures = []
    assets_to_save = []
    imported_loaded_count = 0
    existing_checked_count = 0
    changed_count = 0
    failed_import_count = 0

    # Process only textures that were actually imported by this run. They must be saved because task.save=False.
    for texture_asset_path, record in new_records_by_asset_path.items():
        texture_asset = get_imported_texture_asset_from_task(
            record.get("import_task"),
            texture_asset_path
        )

        if not texture_asset:
            failed_import_count += 1
            unreal.log_warning(
                f"TEXTURE DIRTY BULK: import task не вернул Texture asset и fallback load failed: "
                f"{texture_asset_path}"
            )
            continue

        imported_loaded_count += 1
        GLOBAL_TEXTURE_ASSET_CACHE[texture_asset_path] = texture_asset
        GLOBAL_TEXTURE_ASSET_CACHE[get_asset_path(texture_asset)] = texture_asset
        processed_textures.append(texture_asset)

        if configure_texture_by_suffix_if_needed(texture_asset, record["texture_name"]):
            changed_count += 1

        # Newly imported assets are dirty because task.save=False, even when suffix settings did not change.
        # Save them in one batch to clear dirty flags for all new textures.
        assets_to_save.append(texture_asset)

    # Optional slow validation path for existing textures.
    # Disabled by default to avoid loading/saving hundreds of old assets on every run.
    if POSTPROCESS_EXISTING_TEXTURES:
        for record in existing_records:
            texture_asset_path = record["asset_path"]
            texture_asset = unreal.load_asset(texture_asset_path)

            if not texture_asset:
                unreal.log_warning(f"TEXTURE DIRTY BULK: не удалось загрузить существующую Texture asset: {texture_asset_path}")
                continue

            existing_checked_count += 1
            GLOBAL_TEXTURE_ASSET_CACHE[texture_asset_path] = texture_asset
            GLOBAL_TEXTURE_ASSET_CACHE[get_asset_path(texture_asset)] = texture_asset
            processed_textures.append(texture_asset)

            if configure_texture_by_suffix_if_needed(texture_asset, record["texture_name"]):
                changed_count += 1
                assets_to_save.append(texture_asset)

    save_loaded_assets_batch_safely(assets_to_save, log_label="Texture assets")

    unreal.log(
        f"TEXTURE DIRTY BULK POST: imported loaded={imported_loaded_count}, "
        f"failed imports={failed_import_count}, existing checked={existing_checked_count}, "
        f"suffix changed={changed_count}, batch save count={len(assets_to_save)}."
    )

    return processed_textures


def collect_unique_texture_paths_from_mesh_metadata(meshes):
    unique_texture_paths = {}

    for mesh in meshes:
        if not isinstance(mesh, (unreal.StaticMesh, unreal.SkeletalMesh)):
            continue

        mesh_path = mesh.get_path_name()

        try:
            metadata_tags = unreal.EditorAssetLibrary.get_metadata_tag_values(mesh)
        except Exception as e:
            unreal.log_warning(
                f"TEXTURE BULK: не удалось получить metadata для '{mesh_path}': {str(e)}"
            )
            continue

        if not metadata_tags:
            continue

        for tag, value in metadata_tags.items():
            tag_str = str(tag)

            if "INTERCHANGE" not in tag_str and "FBX." not in tag_str:
                continue

            metadata = decode_json_safely(value)

            if metadata is None:
                continue

            shader_class, textures, optional_parameters = parse_material_metadata(metadata)

            for tex_path in textures.values():
                normalized_path = normalize_texture_source_path(tex_path)

                if normalized_path:
                    unique_texture_paths.setdefault(normalized_path, normalized_path)

    return list(unique_texture_paths.values())


def bulk_prepare_textures_for_meshes(meshes, destination_folder):
    texture_paths = collect_unique_texture_paths_from_mesh_metadata(meshes)

    unreal.log(
        f"TEXTURE BULK COLLECT: собрано уникальных source texture paths из metadata: "
        f"{len(texture_paths)}."
    )

    if not texture_paths:
        return []

    return import_textures(texture_paths, destination_folder)


def load_texture_asset_cached(texture_asset_path):
    texture_asset = GLOBAL_TEXTURE_ASSET_CACHE.get(texture_asset_path)

    if texture_asset:
        return texture_asset

    texture_asset = unreal.load_asset(texture_asset_path)

    if texture_asset:
        GLOBAL_TEXTURE_ASSET_CACHE[texture_asset_path] = texture_asset

    return texture_asset


# ============================================================
# MATERIAL INSTANCES
# ============================================================

def load_master_material(shader_class):
    master_material_path = f"{MASTER_MATERIAL_FOLDER}/{shader_class}"
    master_material = unreal.load_asset(master_material_path)

    if not master_material:
        unreal.log_error(f"Master material не найден по пути: {master_material_path}")
        return None

    return master_material


def get_material_instance_parent(material_instance):
    if not material_instance:
        return None

    try:
        return material_instance.get_editor_property("parent")
    except Exception:
        return None


def set_material_instance_parent_if_needed(material_instance, master_material):
    if not material_instance or not master_material:
        return False

    current_parent = get_material_instance_parent(material_instance)

    if is_same_asset(current_parent, master_material):
        return False

    old_parent_path = get_asset_path(current_parent)
    new_parent_path = get_asset_path(master_material)

    try:
        material_instance.set_editor_property("parent", master_material)
        unreal.log(
            f"ОБНОВЛЁН parent MI '{material_instance.get_name()}': "
            f"{old_parent_path} -> {new_parent_path}"
        )
        return True

    except Exception as e:
        unreal.log_warning(
            f"Не удалось обновить parent у MI '{material_instance.get_name()}': {str(e)}"
        )
        return False


def create_or_load_material_instance(material_name, shader_class, mesh_folder):
    """
    Только создаёт или загружает MI.
    ВАЖНО: существующий MI здесь НЕ обновляется по параметрам и НЕ меняет parent.
    Решение о rebuild принимается позже, после проверки slot material и parent.
    """
    material_instance_folder = f"{mesh_folder}/{MATERIALS_SUBFOLDER_NAME}"
    ensure_directory(material_instance_folder)

    material_instance_path = f"{material_instance_folder}/{material_name}"
    master_material = load_master_material(shader_class)

    if not master_material:
        return None, None, False

    is_new = False

    if unreal.EditorAssetLibrary.does_asset_exist(material_instance_path):
        material_instance = unreal.load_asset(material_instance_path)

        if not isinstance(material_instance, unreal.MaterialInstanceConstant):
            unreal.log_error(
                f"Asset уже существует по пути '{material_instance_path}', "
                f"но это не MaterialInstanceConstant. Обновление невозможно."
            )
            return None, None, False

        unreal.log(f"Найден существующий Material Instance: {material_instance_path}")

    else:
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

        material_instance = asset_tools.create_asset(
            material_name,
            material_instance_folder,
            unreal.MaterialInstanceConstant,
            unreal.MaterialInstanceConstantFactoryNew()
        )

        if material_instance:
            material_instance.set_editor_property("parent", master_material)
            unreal.log(f"Создан новый Material Instance: {material_instance.get_path_name()}")
            is_new = True

    return material_instance, master_material, is_new


def update_material_instance_parameters(material_instance, optional_parameters, force_update=True):
    if not optional_parameters:
        return False

    changed = False

    for param_name, param_value in optional_parameters.items():
        try:
            if isinstance(param_value, list) and len(param_value) == 4:
                current_value = unreal.MaterialEditingLibrary.get_material_instance_vector_parameter_value(
                    material_instance,
                    param_name
                )

                new_value = unreal.LinearColor(
                    param_value[0],
                    param_value[1],
                    param_value[2],
                    param_value[3]
                )

                if force_update or current_value != new_value:
                    unreal.MaterialEditingLibrary.set_material_instance_vector_parameter_value(
                        material_instance,
                        param_name,
                        new_value
                    )
                    changed = True
                    unreal.log(
                        f"ОБНОВЛЁН vector parameter '{param_name}' "
                        f"на MI '{material_instance.get_name()}'"
                    )

            elif isinstance(param_value, (float, int)):
                current_value = unreal.MaterialEditingLibrary.get_material_instance_scalar_parameter_value(
                    material_instance,
                    param_name
                )

                new_value = float(param_value)

                if force_update or current_value != new_value:
                    unreal.MaterialEditingLibrary.set_material_instance_scalar_parameter_value(
                        material_instance,
                        param_name,
                        new_value
                    )
                    changed = True
                    unreal.log(
                        f"ОБНОВЛЁН scalar parameter '{param_name}' "
                        f"на MI '{material_instance.get_name()}' = {new_value}"
                    )

        except Exception as e:
            unreal.log_warning(
                f"Не удалось назначить параметр '{param_name}' "
                f"на Material Instance '{material_instance.get_name()}': {str(e)}"
            )

    return changed


def set_texture_parameters(material_instance, textures, texture_folder, force_update=True):
    if not textures:
        return False

    changed = False

    for tex_key, tex_path in textures.items():
        if not tex_path:
            continue

        tex_path = normalize_texture_source_path(tex_path)
        texture_asset_path, texture_name = get_texture_asset_path_from_source_path(
            tex_path,
            texture_folder
        )

        if unreal.EditorAssetLibrary.does_asset_exist(texture_asset_path):
            texture_asset = load_texture_asset_cached(texture_asset_path)

            if texture_asset:
                try:
                    current_texture = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(
                        material_instance,
                        tex_key
                    )

                    if force_update or not is_same_asset(current_texture, texture_asset):
                        unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(
                            material_instance,
                            tex_key,
                            texture_asset
                        )
                        changed = True
                        unreal.log(
                            f"ОБНОВЛЁН texture parameter '{tex_key}' "
                            f"на MI '{material_instance.get_name()}': {texture_asset_path}"
                        )

                except Exception as e:
                    unreal.log_warning(
                        f"Не удалось назначить texture parameter '{tex_key}' "
                        f"на Material Instance '{material_instance.get_name()}': {str(e)}"
                    )

        else:
            unreal.log_warning(f"Текстура не найдена для назначения: {texture_asset_path}")

    return changed


def rebuild_material_instance_from_metadata(material_instance, master_material, textures, optional_parameters, is_new, reason):
    """
    Полный rebuild MI по metadata.
    Вызывается ТОЛЬКО если материал реально нуждается в обновлении:
    - новый MI
    - неправильный материал в mesh slot
    - неправильный parent у MI
    """
    if not material_instance:
        return False

    if not REBUILD_WRONG_MATERIAL_INSTANCES_FROM_METADATA and not is_new:
        unreal.log(
            f"Пропуск rebuild MI '{material_instance.get_name()}': "
            f"REBUILD_WRONG_MATERIAL_INSTANCES_FROM_METADATA = False"
        )
        return False

    unreal.log(
        f"REBUILD MI '{material_instance.get_name()}' по metadata. Причина: {reason}"
    )

    changed = False

    if master_material:
        if set_material_instance_parent_if_needed(material_instance, master_material):
            changed = True

    # Textures are imported/configured once globally in main_process() before MI rebuild.
    # Do not run per-MI texture imports here; it causes repeated small imports and point saves.
    if set_texture_parameters(
        material_instance,
        textures,
        COMMON_TEXTURE_FOLDER,
        force_update=True
    ):
        changed = True

    if update_material_instance_parameters(
        material_instance,
        optional_parameters,
        force_update=True
    ):
        changed = True

    if changed or is_new:
        try:
            unreal.EditorAssetLibrary.save_loaded_asset(material_instance)
            unreal.log(f"Сохранён Material Instance: {material_instance.get_path_name()}")
        except Exception as e:
            unreal.log_warning(
                f"Не удалось сохранить MI '{material_instance.get_path_name()}': {str(e)}"
            )

    return changed or is_new


# ============================================================
# MESH / COMPONENT HELPERS
# ============================================================

def get_skeletal_mesh_from_component(comp):
    """
    В разных версиях UE5 SkeletalMeshComponent может хранить asset
    в разных editor property:
    - skeletal_mesh_asset
    - skeletal_mesh
    """
    for prop_name in ["skeletal_mesh_asset", "skeletal_mesh"]:
        try:
            mesh = comp.get_editor_property(prop_name)

            if mesh:
                return mesh

        except Exception:
            pass

    return None


def get_material_slot_info(mesh, index, mat_info=None):
    """
    Возвращает текущий материал слота для логов и сравнения.
    """
    try:
        if isinstance(mesh, unreal.StaticMesh):
            return mesh.get_material(index)

        if isinstance(mesh, unreal.SkeletalMesh) and mat_info:
            return mat_info.material_interface

    except Exception:
        return None

    return None


def describe_material_replacement(current_material, new_material):
    if not current_material:
        return "slot был пустой"

    if is_same_asset(current_material, new_material):
        return "slot уже содержит корректный Material Instance"

    current_type = current_material.get_class().get_name() if current_material else "None"
    new_type = new_material.get_class().get_name() if new_material else "None"

    return (
        f"slot обновлён: {current_material.get_name()} ({current_type}) "
        f"-> {new_material.get_name()} ({new_type})"
    )


# ============================================================
# MESH PROCESSING
# ============================================================

def process_mesh_metadata(mesh):
    """
    Читает metadata у StaticMesh/SkeletalMesh asset.

    Новая логика обновления:
    - если slot уже содержит корректный MI и у MI корректный parent — НЕ трогаем параметры;
    - если slot содержит master material / wrong material / empty slot — rebuild MI по metadata;
    - если существующий MI имеет неправильный parent — rebuild MI по metadata;
    - новый MI всегда собирается полностью по metadata.
    """
    if not isinstance(mesh, (unreal.StaticMesh, unreal.SkeletalMesh)):
        return

    mesh_path = mesh.get_path_name()
    mesh_folder = os.path.dirname(mesh_path)

    unreal.log(f"--- Обработка mesh asset: {mesh_path} ---")

    try:
        metadata_tags = unreal.EditorAssetLibrary.get_metadata_tag_values(mesh)
    except Exception as e:
        unreal.log_warning(f"Не удалось получить metadata для '{mesh_path}': {str(e)}")
        return

    if not metadata_tags:
        unreal.log_warning(f"Metadata не найдена у mesh asset: {mesh_path}")
        return

    ensure_directory(COMMON_TEXTURE_FOLDER)

    mesh_was_modified = False
    found_usable_metadata = False

    for tag, value in metadata_tags.items():
        tag_str = str(tag)

        # Сохраняем исходную логику:
        # работаем только с INTERCHANGE или FBX. metadata tags.
        if "INTERCHANGE" not in tag_str and "FBX." not in tag_str:
            continue

        found_usable_metadata = True
        target_slot_name = tag_str.split('.')[-1]

        metadata = decode_json_safely(value)

        if metadata is None:
            continue

        shader_class, textures, optional_parameters = parse_material_metadata(metadata)

        unreal.log(
            f"Metadata slot '{target_slot_name}': "
            f"master='{shader_class}', textures={len(textures)}, params={len(optional_parameters)}"
        )

        material_instance, master_material, is_new = create_or_load_material_instance(
            target_slot_name,
            shader_class,
            mesh_folder
        )

        if not material_instance:
            continue

        current_parent = get_material_instance_parent(material_instance)
        parent_is_correct = is_same_asset(current_parent, master_material)
        parent_needs_update = not parent_is_correct

        assigned = False
        rebuild_needed = bool(is_new or parent_needs_update)
        rebuild_reasons = []

        if is_new:
            rebuild_reasons.append("создан новый MI")

        if parent_needs_update:
            rebuild_reasons.append(
                f"неправильный parent MI: {get_asset_path(current_parent)} -> {get_asset_path(master_material)}"
            )

        # --------------------------------------------------------
        # StaticMesh material assignment
        # --------------------------------------------------------
        if isinstance(mesh, unreal.StaticMesh):
            try:
                mesh_materials = mesh.static_materials

                for index, mat_info in enumerate(mesh_materials):
                    current_slot_name = str(mat_info.material_slot_name)

                    if current_slot_name.strip() == target_slot_name.strip():
                        current_material = get_material_slot_info(mesh, index)

                        if not is_same_asset(current_material, material_instance):
                            rebuild_needed = True
                            rebuild_reasons.append(
                                f"slot содержит неправильный материал: {get_asset_path(current_material)}"
                            )

                            mesh.set_material(index, material_instance)
                            mesh_was_modified = True

                            unreal.log(
                                f"ОБНОВЛЁН материал StaticMesh '{mesh.get_name()}', "
                                f"slot '{target_slot_name}': "
                                f"{describe_material_replacement(current_material, material_instance)}"
                            )
                        else:
                            unreal.log(
                                f"Материал StaticMesh '{mesh.get_name()}', slot '{target_slot_name}' "
                                f"уже назначен корректным MI."
                            )

                        assigned = True
                        break

            except Exception as e:
                unreal.log_warning(
                    f"Ошибка назначения материала на StaticMesh '{mesh.get_name()}': {str(e)}"
                )

        # --------------------------------------------------------
        # SkeletalMesh material assignment
        # --------------------------------------------------------
        elif isinstance(mesh, unreal.SkeletalMesh):
            try:
                mesh_materials = mesh.get_editor_property("materials")

                for index, mat_info in enumerate(mesh_materials):
                    current_slot_name = str(mat_info.material_slot_name)

                    if current_slot_name.strip() == target_slot_name.strip():
                        current_material = get_material_slot_info(mesh, index, mat_info)

                        if not is_same_asset(current_material, material_instance):
                            rebuild_needed = True
                            rebuild_reasons.append(
                                f"slot содержит неправильный материал: {get_asset_path(current_material)}"
                            )

                            # Важно: mat_info — копия структуры.
                            # Меняем копию и кладём обратно в массив.
                            mat_info.material_interface = material_instance
                            mesh_materials[index] = mat_info
                            mesh_was_modified = True

                            unreal.log(
                                f"ОБНОВЛЁН материал SkeletalMesh '{mesh.get_name()}', "
                                f"slot '{target_slot_name}': "
                                f"{describe_material_replacement(current_material, material_instance)}"
                            )
                        else:
                            unreal.log(
                                f"Материал SkeletalMesh '{mesh.get_name()}', slot '{target_slot_name}' "
                                f"уже назначен корректным MI."
                            )

                        assigned = True
                        break

                if assigned and mesh_was_modified:
                    mesh.set_editor_property("materials", mesh_materials)

            except Exception as e:
                unreal.log_warning(
                    f"Ошибка назначения материала на SkeletalMesh '{mesh.get_name()}': {str(e)}"
                )

        if not assigned:
            unreal.log_warning(
                f"Слот '{target_slot_name}' не найден в mesh asset '{mesh.get_name()}'."
            )
            # Если slot не найден, не rebuild-им параметры существующего MI: непонятно, где он должен применяться.
            continue

        if rebuild_needed:
            reason = "; ".join(rebuild_reasons) if rebuild_reasons else "требуется rebuild"
            rebuild_material_instance_from_metadata(
                material_instance,
                master_material,
                textures,
                optional_parameters,
                is_new,
                reason
            )
        else:
            unreal.log(
                f"ПРОПУСК параметров MI '{material_instance.get_name()}': "
                f"slot уже корректный, parent уже корректный, rebuild не требуется."
            )

    if not found_usable_metadata:
        unreal.log_warning(
            f"У mesh asset '{mesh.get_name()}' есть metadata, но нет подходящих тегов INTERCHANGE/FBX."
        )

    if mesh_was_modified:
        try:
            unreal.EditorAssetLibrary.save_loaded_asset(mesh)
            unreal.log(f"Сохранён mesh asset: {mesh_path}")

        except Exception as e:
            unreal.log_warning(f"Не удалось сохранить mesh asset '{mesh_path}': {str(e)}")



# ============================================================
# DECAL COMPONENT SHADOW SETTINGS
# ============================================================

def normalize_component_or_asset_name_for_suffix_check(name_value):
    """
    UE иногда добавляет служебные хвосты к component name.
    Для проверки decal/decalS суффикса убираем самые частые технические хвосты.
    """
    if name_value is None:
        return ""

    name = str(name_value).strip().lower()

    technical_suffixes = (
        "_gen_variable",
        "_template",
    )

    changed = True
    while changed:
        changed = False
        for suffix in technical_suffixes:
            if name.endswith(suffix):
                name = name[:-len(suffix)]
                changed = True

    return name


def name_has_decal_suffix(name_value):
    name = normalize_component_or_asset_name_for_suffix_check(name_value)
    return any(name.endswith(suffix) for suffix in DECAL_STATIC_MESH_NAME_SUFFIXES)


def static_mesh_component_is_decal_component(comp):
    """
    Decal static mesh определяем по имени компонента ИЛИ имени назначенного StaticMesh asset.
    Это удобнее в продакшене: иногда компонент называется корректно, иногда только mesh asset.
    """
    if not isinstance(comp, unreal.StaticMeshComponent):
        return False, ""

    comp_name = ""
    mesh_name = ""

    try:
        comp_name = comp.get_name()
    except Exception:
        comp_name = ""

    if name_has_decal_suffix(comp_name):
        return True, "component name"

    try:
        mesh = comp.get_editor_property("static_mesh")
        if mesh:
            mesh_name = mesh.get_name()
    except Exception:
        mesh_name = ""

    if name_has_decal_suffix(mesh_name):
        return True, "static mesh asset name"

    return False, ""


def component_casts_shadow(comp):
    try:
        return bool(comp.get_editor_property("cast_shadow"))
    except Exception:
        pass

    try:
        return bool(comp.cast_shadow)
    except Exception:
        pass

    return None


def set_component_cast_shadow(comp, enabled):
    """
    В UE Python обычно достаточно cast_shadow.
    На всякий случай оставлены fallback-варианты для разных версий UE5.
    """
    try:
        comp.set_editor_property("cast_shadow", bool(enabled))
        return True
    except Exception:
        pass

    try:
        comp.set_cast_shadow(bool(enabled))
        return True
    except Exception:
        pass

    return False


def disable_shadow_for_decal_static_mesh_component(comp, owner_label):
    if not DISABLE_SHADOWS_FOR_DECAL_STATIC_MESH_COMPONENTS:
        return False

    if not isinstance(comp, unreal.StaticMeshComponent):
        return False

    is_decal, reason = static_mesh_component_is_decal_component(comp)
    if not is_decal:
        return False

    current_cast_shadow = component_casts_shadow(comp)

    if current_cast_shadow is False:
        unreal.log(
            f"Пропуск shadows: StaticMeshComponent '{owner_label} / {comp.get_name()}' "
            f"уже Cast Shadow = False. Причина: {reason}."
        )
        return False

    if set_component_cast_shadow(comp, False):
        unreal.log(
            f"SHADOWS OFF: StaticMeshComponent '{owner_label} / {comp.get_name()}' "
            f"получил Cast Shadow = False. Причина: {reason}, suffix _decal/_decals."
        )
        return True

    unreal.log_warning(
        f"Не удалось отключить Cast Shadow у StaticMeshComponent "
        f"'{owner_label} / {comp.get_name()}'."
    )
    return False


# ============================================================
# UCX EMPTY SCENE COMPONENT CLEANUP
# ============================================================

def normalize_component_name_for_ucx_check(name_value):
    if name_value is None:
        return ""

    name = str(name_value).strip()

    # Blueprint component template names may get editor suffixes.
    technical_suffixes = (
        "_GEN_VARIABLE",
        "_Template",
        "_TEMPLATE",
    )

    changed = True
    while changed:
        changed = False
        for suffix in technical_suffixes:
            if name.endswith(suffix):
                name = name[:-len(suffix)]
                changed = True

    return name


def name_has_ucx_prefix(name_value):
    name = normalize_component_name_for_ucx_check(name_value).lower()
    return any(name.startswith(prefix.lower()) for prefix in UCX_SCENE_COMPONENT_PREFIXES)


def is_plain_scene_component(comp):
    """
    UCX cleanup is intentionally conservative.
    We delete only plain SceneComponent-style empty wrappers.
    StaticMeshComponent / SkeletalMeshComponent / PrimitiveComponent are skipped.
    """
    if not isinstance(comp, unreal.SceneComponent):
        return False

    if isinstance(comp, (unreal.StaticMeshComponent, unreal.SkeletalMeshComponent)):
        return False

    try:
        if hasattr(unreal, "PrimitiveComponent") and isinstance(comp, unreal.PrimitiveComponent):
            return False
    except Exception:
        pass

    return True


def is_ucx_scene_component_to_clean(comp):
    if not CLEAN_UCX_SCENE_COMPONENTS:
        return False

    if not is_plain_scene_component(comp):
        return False

    try:
        comp_name = comp.get_name()
    except Exception:
        comp_name = ""

    return name_has_ucx_prefix(comp_name)


def get_component_children_count(comp):
    try:
        children = comp.get_children_components(True)
        if children:
            return len(children)
    except Exception:
        pass
    return 0


def delete_ucx_scene_components_from_blueprint(bp, subsystem, handles):
    """
    Deletes Blueprint subobjects whose associated object is a plain SceneComponent
    and whose name starts with UCX_.
    """
    if not CLEAN_UCX_SCENE_COMPONENTS:
        return 0

    if not handles:
        return 0

    context_handle = handles[0]
    handles_to_delete = []
    names_to_delete = []

    for handle in handles:
        try:
            data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(handle)
        except Exception:
            continue

        comp = get_associated_object_from_subobject_data(data)

        if not comp:
            continue

        if is_ucx_scene_component_to_clean(comp):
            handles_to_delete.append(handle)
            names_to_delete.append(comp.get_name())

    if not handles_to_delete:
        return 0

    deleted_count = 0

    try:
        deleted_count = subsystem.delete_subobjects(context_handle, handles_to_delete, bp)
    except Exception as e_delete_many:
        unreal.log_warning(
            f"delete_subobjects для Blueprint '{bp.get_name()}' не сработал, "
            f"пробую удалить UCX компоненты по одному. Ошибка: {str(e_delete_many)}"
        )

        for handle, comp_name in zip(handles_to_delete, names_to_delete):
            try:
                deleted_count += subsystem.delete_subobject(context_handle, handle, bp)
            except Exception as e_delete_one:
                unreal.log_warning(
                    f"Не удалось удалить UCX SceneComponent '{comp_name}' "
                    f"из Blueprint '{bp.get_name()}': {str(e_delete_one)}"
                )

    for comp_name in names_to_delete:
        unreal.log(
            f"UCX CLEANUP: Blueprint '{bp.get_name()}' -> удалён SceneComponent '{comp_name}'."
        )

    return int(deleted_count or 0)


def delete_ucx_scene_components_from_actor_direct_fallback(actor):
    """
    Last-resort deletion path for Actor instance components.
    The preferred path is SubobjectDataSubsystem.k2_delete_subobjects_from_instance(),
    because this is the same editor-side subobject system that the Components panel uses.
    """
    deleted_count = 0

    try:
        scene_components = actor.get_components_by_class(unreal.SceneComponent)
    except Exception as e:
        unreal.log_warning(
            f"Не удалось получить SceneComponent у Actor '{actor.get_name()}' "
            f"для direct UCX fallback: {str(e)}"
        )
        return 0

    for comp in list(scene_components):
        if not comp or not is_ucx_scene_component_to_clean(comp):
            continue

        comp_name = comp.get_name()
        child_count = get_component_children_count(comp)

        try:
            actor.modify()
        except Exception:
            pass

        try:
            comp.modify()
        except Exception:
            pass

        deleted = False
        last_error = None

        # Manually added actor components are usually stored in InstanceComponents.
        # Removing from this array is required in many editor cases; otherwise the
        # component may survive in the Components panel even after DestroyComponent().
        try:
            actor.remove_instance_component(comp)
        except Exception:
            pass

        try:
            comp.destroy_component(False)
            deleted = True
        except Exception as e_with_arg:
            last_error = e_with_arg
            try:
                comp.destroy_component()
                deleted = True
            except Exception as e_no_arg:
                last_error = e_no_arg

        if deleted:
            deleted_count += 1
            if child_count > 0:
                unreal.log_warning(
                    f"UCX CLEANUP DIRECT: Actor '{actor.get_name()}' -> удалён SceneComponent "
                    f"'{comp_name}', у него было child components: {child_count}."
                )
            else:
                unreal.log(
                    f"UCX CLEANUP DIRECT: Actor '{actor.get_name()}' -> удалён SceneComponent '{comp_name}'."
                )
        else:
            unreal.log_warning(
                f"DIRECT fallback не смог удалить UCX SceneComponent "
                f"'{actor.get_name()} / {comp_name}': {str(last_error)}"
            )

    return deleted_count


def delete_ucx_scene_components_from_actor(actor):
    """
    ALWAYS cleanup UCX_* SceneComponents from the selected Actor instance.

    Important:
    - This runs for plain Actors AND Blueprint Actor instances.
    - It does not depend on material processing or mesh detection.
    - Preferred deletion path uses SubobjectDataSubsystem instance APIs:
      k2_gather_subobject_data_for_instance + k2_delete_subobjects_from_instance.
      Epic documents that SubobjectDataSubsystem can gather subobjects from an
      actor instance, not only from a Blueprint asset.
    - No rerun_construction_scripts() here: rerunning construction scripts can
      recreate editor-side components and make it look as if deletion failed.
    """
    if not CLEAN_UCX_SCENE_COMPONENTS:
        return 0

    if not isinstance(actor, unreal.Actor):
        return 0

    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)

    # --------------------------------------------------------
    # 1) Preferred editor path: delete subobjects from INSTANCE
    # --------------------------------------------------------
    deleted_count = 0
    handles_to_delete = []
    names_to_delete = []
    context_handle = None

    try:
        handles = subsystem.k2_gather_subobject_data_for_instance(actor)
        if handles:
            context_handle = handles[0]

            for handle in handles:
                try:
                    data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(handle)
                except Exception:
                    try:
                        data = subsystem.k2_find_subobject_data_from_handle(handle)
                    except Exception:
                        data = None

                if not data:
                    continue

                comp = get_associated_object_from_subobject_data(data)
                if not comp:
                    continue

                if is_ucx_scene_component_to_clean(comp):
                    handles_to_delete.append(handle)
                    names_to_delete.append(comp.get_name())

            if handles_to_delete:
                try:
                    actor.modify()
                except Exception:
                    pass

                try:
                    deleted_count = subsystem.k2_delete_subobjects_from_instance(
                        context_handle,
                        handles_to_delete
                    )
                except Exception as e_delete_many:
                    unreal.log_warning(
                        f"k2_delete_subobjects_from_instance для Actor '{actor.get_name()}' не сработал, "
                        f"пробую удалить UCX components по одному. Ошибка: {str(e_delete_many)}"
                    )
                    deleted_count = 0

                    for handle, comp_name in zip(handles_to_delete, names_to_delete):
                        try:
                            deleted_count += subsystem.k2_delete_subobject_from_instance(
                                context_handle,
                                handle
                            )
                        except Exception as e_delete_one:
                            unreal.log_warning(
                                f"k2_delete_subobject_from_instance не смог удалить "
                                f"'{actor.get_name()} / {comp_name}': {str(e_delete_one)}"
                            )

                if deleted_count:
                    for comp_name in names_to_delete:
                        unreal.log(
                            f"UCX CLEANUP INSTANCE: Actor '{actor.get_name()}' -> удалён SceneComponent '{comp_name}'."
                        )

    except Exception as e:
        unreal.log_warning(
            f"SubobjectDataSubsystem instance cleanup не смог обработать Actor "
            f"'{actor.get_name()}': {str(e)}"
        )

    # --------------------------------------------------------
    # 2) Verification + fallback.
    # Sometimes UE returns 0 or the Components panel keeps stale data.
    # Re-scan actor components and delete any remaining UCX_* directly.
    # --------------------------------------------------------
    remaining_ucx = []
    try:
        for comp in actor.get_components_by_class(unreal.SceneComponent):
            if comp and is_ucx_scene_component_to_clean(comp):
                remaining_ucx.append(comp.get_name())
    except Exception:
        remaining_ucx = []

    if remaining_ucx:
        unreal.log_warning(
            f"UCX CLEANUP VERIFY: у Actor '{actor.get_name()}' всё ещё найдены UCX components "
            f"после instance delete: {remaining_ucx}. Запускаю direct fallback."
        )
        deleted_count += delete_ucx_scene_components_from_actor_direct_fallback(actor)

    # Mark actor dirty so the level knows the component hierarchy changed.
    if deleted_count > 0:
        try:
            actor.modify()
        except Exception:
            pass

    return int(deleted_count or 0)


# ============================================================
# STATIC MESH BUILD SETTINGS
# ============================================================

def get_static_mesh_editor_subsystem_safely():
    try:
        return unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    except Exception:
        return None


def get_static_mesh_lod_count(static_mesh, static_mesh_subsystem):
    if static_mesh_subsystem:
        try:
            lod_count = static_mesh_subsystem.get_lod_count(static_mesh)
            if lod_count and lod_count > 0:
                return int(lod_count)
        except Exception:
            pass

    try:
        lod_count = static_mesh.get_num_source_models()
        if lod_count and lod_count > 0:
            return int(lod_count)
    except Exception:
        pass

    return 1


def set_static_mesh_max_lumen_mesh_cards(static_mesh, target_cards=TARGET_MAX_LUMEN_MESH_CARDS, save_asset=True):
    """
    Sets MeshBuildSettings.max_lumen_mesh_cards for every LOD of a StaticMesh.
    Preferred route: StaticMeshEditorSubsystem.get_lod_build_settings / set_lod_build_settings.
    Fallback route: source_model.build_settings.
    If save_asset is False, the StaticMesh is only marked dirty and should be batch-saved by the caller.
    """
    if not SET_MAX_LUMEN_MESH_CARDS_FOR_STATIC_MESHES:
        return False

    if not isinstance(static_mesh, unreal.StaticMesh):
        return False

    static_mesh_subsystem = get_static_mesh_editor_subsystem_safely()
    lod_count = get_static_mesh_lod_count(static_mesh, static_mesh_subsystem)

    mesh_modified = False
    updated_lods = []

    for lod_index in range(lod_count):
        build_settings = None
        route = "StaticMeshEditorSubsystem"

        # Preferred UE5 editor subsystem route.
        if static_mesh_subsystem:
            try:
                build_settings = static_mesh_subsystem.get_lod_build_settings(static_mesh, lod_index)
            except Exception:
                build_settings = None

        # Fallback: source model route.
        if build_settings is None:
            route = "source_model.build_settings"
            try:
                source_model = static_mesh.get_source_model(lod_index)
                build_settings = source_model.get_editor_property("build_settings")
            except Exception:
                build_settings = None

        if build_settings is None:
            unreal.log_warning(
                f"LUMEN CARDS: не удалось получить MeshBuildSettings для "
                f"StaticMesh '{static_mesh.get_name()}', LOD {lod_index}."
            )
            continue

        try:
            current_value = int(build_settings.get_editor_property("max_lumen_mesh_cards"))
        except Exception as e:
            unreal.log_warning(
                f"LUMEN CARDS: MeshBuildSettings у StaticMesh '{static_mesh.get_name()}' "
                f"не содержит max_lumen_mesh_cards. Ошибка: {str(e)}"
            )
            continue

        if current_value == int(target_cards):
            unreal.log(
                f"LUMEN CARDS SKIP: StaticMesh '{static_mesh.get_name()}', "
                f"LOD {lod_index} уже Max Lumen Mesh Cards = {target_cards}."
            )
            continue

        try:
            build_settings.set_editor_property("max_lumen_mesh_cards", int(target_cards))

            if static_mesh_subsystem:
                static_mesh_subsystem.set_lod_build_settings(static_mesh, lod_index, build_settings)
            else:
                source_model = static_mesh.get_source_model(lod_index)
                source_model.set_editor_property("build_settings", build_settings)

            mesh_modified = True
            updated_lods.append(lod_index)

            unreal.log(
                f"LUMEN CARDS UPDATE: StaticMesh '{static_mesh.get_name()}', "
                f"LOD {lod_index}: {current_value} -> {target_cards} ({route})."
            )

        except Exception as e:
            unreal.log_warning(
                f"LUMEN CARDS: не удалось установить Max Lumen Mesh Cards "
                f"для StaticMesh '{static_mesh.get_name()}', LOD {lod_index}: {str(e)}"
            )

    if mesh_modified:
        try:
            static_mesh.mark_package_dirty()
        except Exception:
            pass

        if save_asset:
            try:
                unreal.EditorAssetLibrary.save_loaded_asset(static_mesh)
                unreal.log(
                    f"LUMEN CARDS SAVE: StaticMesh '{static_mesh.get_path_name()}' сохранён. "
                    f"Обновлены LOD: {updated_lods}."
                )
            except Exception as e:
                unreal.log_warning(
                    f"LUMEN CARDS: StaticMesh '{static_mesh.get_name()}' изменён, "
                    f"но сохранить не удалось: {str(e)}"
                )
        else:
            unreal.log(
                f"LUMEN CARDS DIRTY: StaticMesh '{static_mesh.get_path_name()}' изменён; "
                f"сохранение будет выполнено batch save. Обновлены LOD: {updated_lods}."
            )

    return mesh_modified


def get_asset_metadata_tag_safely(asset, tag_name, default=""):
    try:
        return str(unreal.EditorAssetLibrary.get_metadata_tag(asset, tag_name))
    except Exception:
        return default


def set_asset_metadata_tag_safely(asset, tag_name, value):
    try:
        unreal.EditorAssetLibrary.set_metadata_tag(asset, tag_name, str(value))
        return True
    except Exception as e:
        unreal.log_warning(
            f"Не удалось установить metadata tag '{tag_name}' для asset "
            f"'{get_asset_path(asset)}': {str(e)}"
        )
        return False


def decal_static_mesh_asset_shadow_marker_exists(static_mesh):
    marker_value = get_asset_metadata_tag_safely(
        static_mesh,
        DECAL_STATIC_MESH_ASSET_SHADOWS_METADATA_TAG,
        default=""
    ).strip().lower()

    return marker_value in ("1", "true", "yes", "done", "applied")


def static_mesh_asset_is_decal_mesh(static_mesh):
    if not isinstance(static_mesh, unreal.StaticMesh):
        return False

    try:
        mesh_name = static_mesh.get_name()
    except Exception:
        mesh_name = ""

    return name_has_decal_suffix(mesh_name)


def get_static_mesh_num_sections_for_lod(static_mesh, lod_index):
    """
    Best-effort section count helper.
    StaticMesh.get_num_sections(lod_index) exists in many UE Python builds, but not all docs expose it.
    """
    for method_name in ("get_num_sections", "get_num_sections_for_lod"):
        try:
            method = getattr(static_mesh, method_name)
        except Exception:
            method = None

        if not method:
            continue

        try:
            section_count = method(lod_index)
            if section_count is not None and int(section_count) >= 0:
                return int(section_count)
        except Exception:
            pass

    return None


def get_static_mesh_material_count(static_mesh, static_mesh_subsystem=None):
    if static_mesh_subsystem:
        try:
            material_count = static_mesh_subsystem.get_number_materials(static_mesh)
            if material_count is not None and int(material_count) >= 0:
                return int(material_count)
        except Exception:
            pass

    try:
        return len(static_mesh.static_materials)
    except Exception:
        pass

    try:
        return int(static_mesh.get_num_sections(0))
    except Exception:
        pass

    return 0


def get_static_mesh_lod_section_indices(static_mesh, static_mesh_subsystem, lod_index):
    section_count = get_static_mesh_num_sections_for_lod(static_mesh, lod_index)

    if section_count is not None:
        return list(range(section_count))

    # Fallback path for UE Python builds where StaticMesh.get_num_sections is unavailable.
    # get_lod_material_slot returns INDEX_NONE / negative for invalid sections.
    section_indices = []
    material_count = get_static_mesh_material_count(static_mesh, static_mesh_subsystem)
    scan_limit = max(material_count, STATIC_MESH_SECTION_SCAN_FALLBACK_LIMIT)

    if static_mesh_subsystem:
        for section_index in range(scan_limit):
            try:
                material_slot_index = static_mesh_subsystem.get_lod_material_slot(
                    static_mesh,
                    lod_index,
                    section_index
                )
            except Exception:
                if section_indices:
                    break
                if section_index >= material_count:
                    break
                continue

            try:
                material_slot_index = int(material_slot_index)
            except Exception:
                material_slot_index = -1

            if material_slot_index < 0:
                if section_indices:
                    break
                if section_index >= material_count:
                    break
                continue

            section_indices.append(section_index)

    if section_indices:
        return section_indices

    # Last conservative fallback: assume section indices match material slots.
    # This is usually true for imported decal meshes and avoids doing nothing.
    if material_count > 0:
        return list(range(material_count))

    return []


def enable_static_mesh_section_cast_shadow(static_mesh_subsystem, static_mesh, cast_shadow, lod_index, section_index):
    if static_mesh_subsystem:
        try:
            static_mesh_subsystem.enable_section_cast_shadow(
                static_mesh,
                bool(cast_shadow),
                int(lod_index),
                int(section_index)
            )
            return True
        except Exception as e:
            subsystem_error = e
    else:
        subsystem_error = None

    # Deprecated fallback, but useful in some editor builds if StaticMeshEditorSubsystem is unavailable.
    try:
        if hasattr(unreal, "EditorStaticMeshLibrary"):
            unreal.EditorStaticMeshLibrary.enable_section_cast_shadow(
                static_mesh,
                bool(cast_shadow),
                int(lod_index),
                int(section_index)
            )
            return True
    except Exception as fallback_error:
        unreal.log_warning(
            f"STATICMESH DECAL SHADOWS: не удалось установить Cast Shadow={cast_shadow} "
            f"для StaticMesh '{static_mesh.get_name()}', LOD {lod_index}, section {section_index}. "
            f"Subsystem error: {str(subsystem_error)}; fallback error: {str(fallback_error)}"
        )
        return False

    unreal.log_warning(
        f"STATICMESH DECAL SHADOWS: StaticMeshEditorSubsystem недоступен и fallback отсутствует. "
        f"StaticMesh '{static_mesh.get_name()}', LOD {lod_index}, section {section_index}."
    )
    return False


def disable_static_mesh_asset_shadows_for_decal_mesh(static_mesh, save_asset=True):
    """
    Disables section-level shadow casting on the StaticMesh asset itself for meshes named *_decal / *_decals.
    This is different from StaticMeshComponent.cast_shadow and applies to LOD sections stored in the asset.
    """
    if not DISABLE_SHADOWS_FOR_DECAL_STATIC_MESH_ASSETS:
        return False

    if not static_mesh_asset_is_decal_mesh(static_mesh):
        return False

    if SKIP_DECAL_STATIC_MESH_ASSET_SHADOWS_IF_MARKER_EXISTS:
        if decal_static_mesh_asset_shadow_marker_exists(static_mesh):
            unreal.log(
                f"STATICMESH DECAL SHADOWS SKIP: StaticMesh '{static_mesh.get_path_name()}' "
                f"уже имеет metadata marker '{DECAL_STATIC_MESH_ASSET_SHADOWS_METADATA_TAG}'."
            )
            return False

    static_mesh_subsystem = get_static_mesh_editor_subsystem_safely()
    lod_count = get_static_mesh_lod_count(static_mesh, static_mesh_subsystem)

    touched_sections = []

    for lod_index in range(lod_count):
        section_indices = get_static_mesh_lod_section_indices(
            static_mesh,
            static_mesh_subsystem,
            lod_index
        )

        if not section_indices:
            unreal.log_warning(
                f"STATICMESH DECAL SHADOWS: не найдены sections для "
                f"StaticMesh '{static_mesh.get_name()}', LOD {lod_index}."
            )
            continue

        for section_index in section_indices:
            if enable_static_mesh_section_cast_shadow(
                static_mesh_subsystem,
                static_mesh,
                False,
                lod_index,
                section_index
            ):
                touched_sections.append((lod_index, section_index))

    if not touched_sections:
        return False

    set_asset_metadata_tag_safely(
        static_mesh,
        DECAL_STATIC_MESH_ASSET_SHADOWS_METADATA_TAG,
        "True"
    )

    try:
        static_mesh.mark_package_dirty()
    except Exception:
        pass

    if save_asset:
        try:
            unreal.EditorAssetLibrary.save_loaded_asset(static_mesh)
            unreal.log(
                f"STATICMESH DECAL SHADOWS SAVE: StaticMesh '{static_mesh.get_path_name()}' сохранён; "
                f"Cast Shadow выключен для sections: {touched_sections}."
            )
        except Exception as e:
            unreal.log_warning(
                f"STATICMESH DECAL SHADOWS: StaticMesh '{static_mesh.get_name()}' изменён, "
                f"но сохранить не удалось: {str(e)}"
            )
    else:
        unreal.log(
            f"STATICMESH DECAL SHADOWS DIRTY: StaticMesh '{static_mesh.get_path_name()}' изменён; "
            f"сохранение будет выполнено batch save. Sections: {touched_sections}."
        )

    return True


# ============================================================
# BLUEPRINT PROCESSING
# ============================================================

def get_associated_object_from_subobject_data(data):
    try:
        return unreal.SubobjectDataBlueprintFunctionLibrary.get_associated_object(data)
    except Exception:
        pass

    try:
        return unreal.SubobjectDataBlueprintFunctionLibrary.get_object(data)
    except Exception:
        return None


def collect_meshes_from_blueprint(bp):
    """
    Обрабатывает Blueprint asset:
    - удаляет plain SceneComponent с именем UCX_*
    - отключает shadows у *_decal / *_decals StaticMeshComponent
    - собирает StaticMesh и SkeletalMesh из компонентов Blueprint
    """
    meshes_to_process = set()
    modified_shadows = False
    deleted_ucx_count = 0

    if not isinstance(bp, unreal.Blueprint):
        return meshes_to_process

    unreal.log(f"--- Обработка Blueprint asset: {bp.get_name()} ---")

    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)

    try:
        handles = subsystem.k2_gather_subobject_data_for_blueprint(bp)
    except Exception as e:
        unreal.log_error(f"Не удалось получить subobject data для Blueprint '{bp.get_name()}': {str(e)}")
        return meshes_to_process

    # First pass: collect meshes / update safe component settings.
    # UCX deletion is done after the pass, so handles stay valid during processing.
    for handle in handles:
        try:
            data = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(handle)
        except Exception:
            continue

        comp = get_associated_object_from_subobject_data(data)

        if not comp:
            continue

        # UCX plain SceneComponent will be deleted later; do not process it as a normal component.
        if is_ucx_scene_component_to_clean(comp):
            continue

        # --------------------------------------------------------
        # StaticMeshComponent
        # --------------------------------------------------------
        if isinstance(comp, unreal.StaticMeshComponent):
            if disable_shadow_for_decal_static_mesh_component(comp, bp.get_name()):
                modified_shadows = True

            try:
                mesh = comp.get_editor_property("static_mesh")

                if mesh:
                    meshes_to_process.add(mesh)

                    unreal.log(
                        f"Найден StaticMesh '{mesh.get_name()}' "
                        f"в Blueprint component '{comp.get_name()}'"
                    )

            except Exception as e:
                unreal.log_warning(
                    f"Не удалось получить StaticMesh из Blueprint component '{comp.get_name()}': {str(e)}"
                )

        # --------------------------------------------------------
        # SkeletalMeshComponent
        # --------------------------------------------------------
        elif isinstance(comp, unreal.SkeletalMeshComponent):
            mesh = get_skeletal_mesh_from_component(comp)

            if mesh:
                meshes_to_process.add(mesh)

                unreal.log(
                    f"Найден SkeletalMesh '{mesh.get_name()}' "
                    f"в Blueprint component '{comp.get_name()}'"
                )

    deleted_ucx_count = delete_ucx_scene_components_from_blueprint(bp, subsystem, handles)

    if modified_shadows or deleted_ucx_count > 0:
        try:
            unreal.BlueprintEditorLibrary.compile_blueprint(bp)
            unreal.EditorAssetLibrary.save_loaded_asset(bp)

            if modified_shadows:
                unreal.log(
                    f"Успех: Blueprint '{bp.get_name()}' скомпилирован, "
                    f"для *_decal/*_decals StaticMeshComponent отключены shadows."
                )

            if deleted_ucx_count > 0:
                unreal.log(
                    f"Успех: Blueprint '{bp.get_name()}' скомпилирован, "
                    f"удалено UCX_* SceneComponent: {deleted_ucx_count}."
                )

        except Exception as e:
            unreal.log_warning(
                f"Blueprint '{bp.get_name()}' изменён, но не удалось compile/save: {str(e)}"
            )

    else:
        unreal.log(
            f"Пропуск: Blueprint '{bp.get_name()}' уже чистый: "
            f"decal shadows выключены, UCX_* SceneComponent не найдены."
        )

    return meshes_to_process


def get_blueprint_asset_from_actor(actor):
    """
    Если выделенный Actor является instance Blueprint-а,
    пытаемся найти сам Blueprint asset.
    Если это обычный Actor, возвращаем None.
    """
    if not isinstance(actor, unreal.Actor):
        return None

    try:
        actor_class = actor.get_class()
        class_path = actor_class.get_path_name()

        # Blueprint generated class обычно выглядит так:
        # /Game/Folder/BP_Name.BP_Name_C
        if class_path.endswith("_C"):
            bp_path = class_path[:-2]
            bp_asset = unreal.load_asset(bp_path)

            if isinstance(bp_asset, unreal.Blueprint):
                return bp_asset

    except Exception as e:
        unreal.log_warning(
            f"Не удалось проверить Blueprint asset для Actor '{actor.get_name()}': {str(e)}"
        )

    return None


# ============================================================
# ACTOR PROCESSING
# ============================================================

def collect_meshes_from_actor(actor):
    """
    Обрабатывает обычный Actor instance во Viewport / World Outliner:
    - удаляет plain SceneComponent с именем UCX_*
    - собирает StaticMesh из StaticMeshComponent
    - собирает SkeletalMesh из SkeletalMeshComponent
    - отключает shadows у *_decal / *_decals StaticMeshComponent
    """
    meshes_to_process = set()
    modified_shadows = False
    deleted_ucx_count = 0

    if not isinstance(actor, unreal.Actor):
        return meshes_to_process, modified_shadows, deleted_ucx_count

    unreal.log(f"--- Обработка обычного Actor instance: {actor.get_name()} ---")

    # --------------------------------------------------------
    # Delete UCX_* plain SceneComponents
    # --------------------------------------------------------
    deleted_ucx_count = delete_ucx_scene_components_from_actor(actor)

    # --------------------------------------------------------
    # StaticMeshComponents
    # --------------------------------------------------------
    try:
        static_mesh_components = actor.get_components_by_class(unreal.StaticMeshComponent)

        for comp in static_mesh_components:
            if not comp:
                continue

            if disable_shadow_for_decal_static_mesh_component(comp, actor.get_name()):
                modified_shadows = True

            try:
                mesh = comp.get_editor_property("static_mesh")

                if mesh:
                    meshes_to_process.add(mesh)

                    unreal.log(
                        f"Найден StaticMesh '{mesh.get_name()}' "
                        f"в Actor component '{actor.get_name()} / {comp.get_name()}'"
                    )

            except Exception as e:
                unreal.log_warning(
                    f"Не удалось получить StaticMesh из Actor component "
                    f"'{actor.get_name()} / {comp.get_name()}': {str(e)}"
                )

    except Exception as e:
        unreal.log_warning(
            f"Не удалось получить StaticMeshComponent у Actor '{actor.get_name()}': {str(e)}"
        )

    # --------------------------------------------------------
    # SkeletalMeshComponents
    # --------------------------------------------------------
    try:
        skeletal_mesh_components = actor.get_components_by_class(unreal.SkeletalMeshComponent)

        for comp in skeletal_mesh_components:
            if not comp:
                continue

            mesh = get_skeletal_mesh_from_component(comp)

            if mesh:
                meshes_to_process.add(mesh)

                unreal.log(
                    f"Найден SkeletalMesh '{mesh.get_name()}' "
                    f"в Actor component '{actor.get_name()} / {comp.get_name()}'"
                )

    except Exception as e:
        unreal.log_warning(
            f"Не удалось получить SkeletalMeshComponent у Actor '{actor.get_name()}': {str(e)}"
        )

    return meshes_to_process, modified_shadows, deleted_ucx_count


def main_process():
    all_meshes_to_process = set()
    processed_blueprints = set()
    processed_actors = set()

    # --------------------------------------------------------
    # 1. Assets selected in Content Browser
    # --------------------------------------------------------
    try:
        selected_assets = unreal.EditorUtilityLibrary.get_selected_assets()
    except Exception as e:
        unreal.log_warning(f"Не удалось получить selected assets из Content Browser: {str(e)}")
        selected_assets = []

    for asset in selected_assets:
        if isinstance(asset, unreal.Blueprint):
            if asset not in processed_blueprints:
                processed_blueprints.add(asset)

                meshes = collect_meshes_from_blueprint(asset)
                all_meshes_to_process.update(meshes)

        elif isinstance(asset, (unreal.StaticMesh, unreal.SkeletalMesh)):
            all_meshes_to_process.add(asset)
            unreal.log(f"Добавлен напрямую выделенный mesh asset: {asset.get_path_name()}")

    # --------------------------------------------------------
    # 2. Actors selected in Viewport / World Outliner
    # --------------------------------------------------------
    try:
        selected_actors = unreal.EditorLevelLibrary.get_selected_level_actors()
    except Exception as e:
        unreal.log_warning(f"Не удалось получить selected actors из Viewport: {str(e)}")
        selected_actors = []

    # --------------------------------------------------------
    # 2.1 ALWAYS cleanup UCX_* components on selected actor instances
    # --------------------------------------------------------
    # This pass is intentionally independent from material/mesh processing.
    # It runs for every selected Actor, including Blueprint Actor instances,
    # even if the Actor is later skipped because its Blueprint asset was already processed.
    for actor in selected_actors:
        if not isinstance(actor, unreal.Actor):
            continue

        deleted_ucx_count_always = delete_ucx_scene_components_from_actor(actor)
        if deleted_ucx_count_always > 0:
            unreal.log(
                f"UCX CLEANUP ALWAYS: Actor '{actor.get_name()}' -> удалено UCX_* SceneComponent: "
                f"{deleted_ucx_count_always}."
            )

    for actor in selected_actors:
        if not isinstance(actor, unreal.Actor):
            continue

        if actor in processed_actors:
            continue

        processed_actors.add(actor)

        # Сначала проверяем, является ли Actor instance Blueprint-а.
        bp_asset = get_blueprint_asset_from_actor(actor)

        if bp_asset:
            # Для Blueprint instance обрабатываем Blueprint asset,
            # чтобы изменения применились на уровне Blueprint.
            # Дополнительно чистим UCX_* components прямо на selected actor instance,
            # потому что UE может хранить такие компоненты как "Source: This actor instance".
            instance_deleted_ucx_count = delete_ucx_scene_components_from_actor(actor)

            if instance_deleted_ucx_count > 0:
                try:
                    actor.modify()
                except Exception:
                    pass

                unreal.log(
                    f"Успех: у Blueprint Actor instance '{actor.get_name()}' удалено "
                    f"UCX_* SceneComponent: {instance_deleted_ucx_count}."
                )

            if bp_asset not in processed_blueprints:
                processed_blueprints.add(bp_asset)

                unreal.log(
                    f"Actor '{actor.get_name()}' является Blueprint instance. "
                    f"Обрабатываем Blueprint asset '{bp_asset.get_name()}'."
                )

                meshes = collect_meshes_from_blueprint(bp_asset)
                all_meshes_to_process.update(meshes)

            else:
                unreal.log(
                    f"Blueprint asset для Actor '{actor.get_name()}' уже был обработан."
                )

        else:
            # Actor НЕ является Blueprint — значит обрабатываем его компоненты напрямую.
            unreal.log(
                f"Actor '{actor.get_name()}' не является Blueprint. "
                f"Обрабатываем его StaticMeshComponent/SkeletalMeshComponent напрямую."
            )

            meshes, modified_shadows, deleted_ucx_count = collect_meshes_from_actor(actor)

            all_meshes_to_process.update(meshes)

            if modified_shadows or deleted_ucx_count > 0:
                try:
                    actor.modify()

                    if modified_shadows:
                        unreal.log(
                            f"Успех: для *_decal/*_decals StaticMeshComponent у Actor "
                            f"'{actor.get_name()}' отключены shadows."
                        )

                    if deleted_ucx_count > 0:
                        unreal.log(
                            f"Успех: у Actor '{actor.get_name()}' удалено "
                            f"UCX_* SceneComponent: {deleted_ucx_count}."
                        )

                except Exception as e:
                    unreal.log_warning(
                        f"Actor '{actor.get_name()}' изменён, "
                        f"но actor.modify() вызвал ошибку: {str(e)}"
                    )

            else:
                unreal.log(
                    f"Пропуск: Actor '{actor.get_name()}' уже чистый: "
                    f"decal shadows выключены, UCX_* SceneComponent не найдены."
                )

    # --------------------------------------------------------
    # 3. Final validation
    # --------------------------------------------------------
    if not all_meshes_to_process:
        unreal.log_warning(
            "Ничего не найдено для обработки. "
            "Выделите Blueprint asset в Content Browser, Blueprint Actor во Viewport, "
            "обычный Actor во Viewport, StaticMesh asset или SkeletalMesh asset."
        )
        return

    # --------------------------------------------------------
    # 4. StaticMesh asset-level processing
    # --------------------------------------------------------
    static_meshes = [mesh for mesh in all_meshes_to_process if isinstance(mesh, unreal.StaticMesh)]

    modified_static_mesh_assets = []

    if static_meshes:
        unreal.log(
            f"STATICMESH PASS: уникальных StaticMesh для asset-level обработки: {len(static_meshes)}. "
            f"Max Lumen Mesh Cards={TARGET_MAX_LUMEN_MESH_CARDS}, "
            f"decal asset shadows off={DISABLE_SHADOWS_FOR_DECAL_STATIC_MESH_ASSETS}."
        )

        for static_mesh in static_meshes:
            if set_static_mesh_max_lumen_mesh_cards(
                static_mesh,
                target_cards=TARGET_MAX_LUMEN_MESH_CARDS,
                save_asset=False
            ):
                modified_static_mesh_assets.append(static_mesh)

            if disable_static_mesh_asset_shadows_for_decal_mesh(
                static_mesh,
                save_asset=False
            ):
                modified_static_mesh_assets.append(static_mesh)

        save_loaded_assets_batch_safely(
            modified_static_mesh_assets,
            log_label="StaticMesh assets"
        )

    # --------------------------------------------------------
    # 5. Global bulk texture import/configuration before material rebuild
    # --------------------------------------------------------
    bulk_prepare_textures_for_meshes(all_meshes_to_process, COMMON_TEXTURE_FOLDER)

    # --------------------------------------------------------
    # 6. Process collected mesh assets materials
    # --------------------------------------------------------
    unreal.log(
        f"Всего уникальных mesh assets для обработки материалов: {len(all_meshes_to_process)}"
    )

    for mesh in all_meshes_to_process:
        process_mesh_metadata(mesh)

    unreal.log("Готово.")


# ============================================================
# RUN
# ============================================================

main_process()
