"""Blender material adapter for the manifest material model (B4-mat).

Dag4blend's ``Material.dagormat`` is the authoring source.  The adapter only
reads its public ``shader_class``, ``sides``, ``optional`` and ``textures``
members; the material node tree is deliberately not a second source of truth.
"""

import ntpath
import os
import posixpath

import bpy

from ..core.model import MaterialResource, MaterialSlot
from ..core.uid import PROP_UID, ensure_uid
from ..core.validate import MHValidationError

__all__ = [
    "DEFAULT_SHADER_CLASS",
    "extract_collection_materials",
    "normalize_texture_path",
    "remap_absolute_texture_path",
    "remap_dagormat_texture_paths",
]

DEFAULT_SHADER_CLASS = "rendinst_simple"


def _json_value(value):
    """Turn Blender ID-property values into their JSON-compatible value.

    Dag4blend stores optional shader parameters as custom properties.  Arrays
    therefore arrive as ``IDPropertyArray``/``bpy_prop_array`` rather than as
    plain Python lists.
    """
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, dict) or hasattr(value, "items"):
        return {
            str(key): _json_value(item)
            for key, item in value.items()
        }
    if hasattr(value, "to_list"):
        return [_json_value(item) for item in value.to_list()]
    if isinstance(value, (list, tuple)):
        return [_json_value(item) for item in value]
    try:
        return [_json_value(item) for item in value]
    except TypeError:
        # Custom ID properties accepted by Blender are expected to be covered
        # above.  Keep failures explicit instead of silently stringifying a
        # value that the UE material builder could not reproduce.
        raise TypeError(
            f"unsupported dagormat optional value {type(value).__name__}")


def _property_items(group):
    if group is None or not hasattr(group, "keys"):
        return []
    return [(str(key), group[key]) for key in group.keys()]


def _windows_absolute(path):
    drive, _tail = ntpath.splitdrive(path)
    return bool(drive) or path.startswith("\\\\")


def _remap_roots_module(old_root, new_root):
    """Validate both roots once and choose their lexical path implementation."""
    old_is_windows = _windows_absolute(old_root)
    new_is_windows = _windows_absolute(new_root)
    old_is_posix = posixpath.isabs(old_root) and not old_is_windows
    new_is_posix = posixpath.isabs(new_root) and not new_is_windows
    if old_is_windows and new_is_windows:
        return ntpath
    if old_is_posix and new_is_posix:
        return posixpath
    raise ValueError(
        "Old Texture Root and Texture Root must be absolute paths of the "
        "same platform style")


def remap_absolute_texture_path(texture_path, old_root, new_root):
    """Map one absolute legacy path from ``old_root`` below ``new_root``.

    Returns ``(path, changed)``. Relative paths, Blender ``//`` paths and
    absolute paths outside ``old_root`` are returned unchanged. Containment
    is lexical and boundary-safe (``C:\\old_extra`` is not below
    ``C:\\old``); Windows paths compare case-insensitively even when tests
    run on another host OS.
    """
    if not isinstance(texture_path, str) or not texture_path:
        return texture_path, False
    if not isinstance(old_root, str) or not old_root.strip():
        raise ValueError("old_root must be a non-empty absolute path")
    if not isinstance(new_root, str) or not new_root.strip():
        raise ValueError("new_root must be a non-empty absolute path")

    # In Blender, two forward slashes always mean blend-file-relative. Do not
    # reinterpret that authoring syntax as a Windows UNC path.
    if texture_path.startswith("//"):
        return texture_path, False

    path = texture_path.strip()
    old = old_root.strip()
    new = new_root.strip()
    path_module = _remap_roots_module(old, new)
    path_is_windows = _windows_absolute(path)
    path_is_posix = posixpath.isabs(path) and not path_is_windows
    if ((path_module is ntpath and not path_is_windows)
            or (path_module is posixpath and not path_is_posix)):
        return texture_path, False

    path = path_module.normpath(path)
    old = path_module.normpath(old)
    new = path_module.normpath(new)
    normalize_case = ntpath.normcase if path_module is ntpath else lambda p: p
    try:
        inside = path_module.commonpath(
            [normalize_case(path), normalize_case(old)]) \
            == normalize_case(old)
    except ValueError:  # different drives or incompatible roots
        inside = False
    if not inside:
        return texture_path, False

    relative = path_module.relpath(path, old)
    mapped = path_module.normpath(path_module.join(new, relative))
    try:
        mapped_inside = path_module.commonpath(
            [normalize_case(mapped), normalize_case(new)]) \
            == normalize_case(new)
    except ValueError:
        mapped_inside = False
    if not mapped_inside:
        return texture_path, False
    if normalize_case(mapped) == normalize_case(path):
        return texture_path, False
    return mapped, True


def remap_dagormat_texture_paths(old_root, new_root, materials=None):
    """Rewrite matching non-empty dagormat texture paths once.

    ``materials`` defaults to all Blender materials and is injectable for UI
    tests. The returned counters let an operator report exactly what happened
    without rescanning or guessing from dirty datablocks.
    """
    # Validate the operation before inspecting or mutating any material.
    _remap_roots_module(str(old_root).strip(), str(new_root).strip())
    if materials is None:
        materials = bpy.data.materials
    counts = {
        "materials_scanned": 0,
        "dagormat_materials": 0,
        "materials_skipped_read_only": 0,
        "materials_changed": 0,
        "texture_paths_scanned": 0,
        "texture_paths_rewritten": 0,
    }
    changes = []
    changed_materials = set()
    for material in materials:
        counts["materials_scanned"] += 1
        dagormat = getattr(material, "dagormat", None)
        texture_group = getattr(dagormat, "textures", None) \
            if dagormat is not None else None
        if texture_group is None:
            continue
        counts["dagormat_materials"] += 1
        if not getattr(material, "is_editable", True):
            counts["materials_skipped_read_only"] += 1
            continue
        for slot, raw_path in _property_items(texture_group):
            if not isinstance(raw_path, str) or not raw_path.strip():
                continue
            counts["texture_paths_scanned"] += 1
            mapped, changed = remap_absolute_texture_path(
                raw_path, old_root, new_root)
            if not changed:
                continue
            changes.append((texture_group, slot, raw_path, mapped))
            changed_materials.add(id(material))

    # Apply only after the full preflight. Direct ID-property assignment keeps
    # this data migration independent from dag4blend's project-specific node
    # rebuild callbacks. If an unexpected Blender write fails, restore every
    # path already changed before surfacing the error to the operator.
    applied = []
    try:
        for texture_group, slot, raw_path, mapped in changes:
            texture_group[slot] = mapped
            applied.append((texture_group, slot, raw_path))
    except Exception:
        for texture_group, slot, raw_path in reversed(applied):
            texture_group[slot] = raw_path
        raise

    counts["materials_changed"] = len(changed_materials)
    counts["texture_paths_rewritten"] = len(changes)
    return counts


def normalize_texture_path(texture_path, texture_root):
    """Return a texture_root-relative, forward-slash path.

    ``//`` keeps its Blender meaning (relative to the current .blend).  Other
    relative strings are already texture-root identities and are resolved
    below ``texture_root`` for the containment check.  Existence is not a
    requirement: source control or a later UE machine may provide the file.
    """
    if not isinstance(texture_path, str):
        return None
    cleaned = os.path.expandvars(
        os.path.expanduser(texture_path.strip().strip('"').strip("'")))
    if not cleaned:
        return None

    if not texture_root:
        raise ValueError("texture_root is required for non-empty textures")
    root = bpy.path.abspath(str(texture_root))
    root = os.path.realpath(os.path.abspath(root))

    if cleaned.startswith("//"):
        absolute = bpy.path.abspath(cleaned)
    elif os.path.isabs(cleaned):
        absolute = cleaned
    else:
        absolute = os.path.join(root, cleaned)
    absolute = os.path.realpath(os.path.abspath(absolute))

    try:
        inside = os.path.commonpath(
            [os.path.normcase(root), os.path.normcase(absolute)]) \
            == os.path.normcase(root)
    except ValueError:  # e.g. different Windows drive letters
        inside = False
    if not inside:
        raise ValueError(f"texture '{texture_path}' is outside '{root}'")

    relative = os.path.relpath(absolute, root).replace("\\", "/")
    if relative in ("", ".") or relative == ".." \
            or relative.startswith("../"):
        raise ValueError(f"texture '{texture_path}' is outside '{root}'")
    return relative


def _material_resource(material, texture_root):
    uid = ensure_uid(material)
    dagormat = getattr(material, "dagormat", None)
    shader_class = ""
    if dagormat is not None:
        raw_shader = getattr(dagormat, "shader_class", "")
        if isinstance(raw_shader, str):
            shader_class = raw_shader.strip()

    # An unconfigured or unavailable dag4blend source intentionally becomes a
    # minimal UE placeholder.  Do not leak stale optional/texture values from
    # a dagormat whose shader_class is empty.
    if not shader_class:
        return MaterialResource(
            uid=uid,
            name=material.name,
            shader_class=DEFAULT_SHADER_CLASS,
            params={},
            textures={},
        )

    params = {}
    optional = getattr(dagormat, "optional", None)
    for key, value in _property_items(optional):
        try:
            params[key] = _json_value(value)
        except (TypeError, ValueError) as exc:
            raise MHValidationError(
                "MH_E_INVALID_MATERIAL_VALUE", [uid], f"{key}: {exc}")
    params["sides"] = int(getattr(dagormat, "sides", 0))

    textures = {}
    texture_group = getattr(dagormat, "textures", None)
    for slot, raw_path in _property_items(texture_group):
        if not isinstance(raw_path, str) or not raw_path.strip():
            continue
        try:
            normalized = normalize_texture_path(raw_path, texture_root)
        except ValueError as exc:
            raise MHValidationError(
                "MH_E_TEXTURE_OUTSIDE_ROOT", [uid], f"{slot}: {exc}")
        if normalized is not None:
            textures[slot] = normalized

    return MaterialResource(
        uid=uid,
        name=material.name,
        shader_class=shader_class,
        params=params,
        textures=textures,
    )


def extract_collection_materials(collection, texture_root=""):
    """Return ``(materials, material_slots)`` for one GEOMETRY resource.

    Mesh objects are ordered by their already-assigned object UID, then by
    Blender slot order.  A MaterialUID contributes at most one table row; its
    first occurrence wins.  Reusing one slot name for different MaterialUIDs
    is ambiguous and therefore blocks the export.
    """
    mesh_objects = [obj for obj in collection.objects if obj.type == "MESH"]
    mesh_objects.sort(key=lambda obj: obj[PROP_UID].encode("utf-8"))

    materials = []
    slots = []
    seen_material_uids = set()
    uid_by_slot_name = {}

    for obj in mesh_objects:
        for slot_index, slot in enumerate(obj.material_slots):
            material = slot.material
            if material is None:
                raise MHValidationError(
                    "MH_E_EMPTY_MATERIAL_SLOT", [obj[PROP_UID]],
                    f"'{obj.name}' material slot {slot_index} is empty")

            resource = _material_resource(material, texture_root)
            material_uid = resource.uid
            slot_name = str(slot.name or material.name)

            previous_uid = uid_by_slot_name.get(slot_name)
            if previous_uid is not None and previous_uid != material_uid:
                raise MHValidationError(
                    "MH_E_MATERIAL_SLOT_CONFLICT",
                    [previous_uid, material_uid],
                    f"slot name '{slot_name}' maps to different materials")
            uid_by_slot_name[slot_name] = material_uid

            if material_uid in seen_material_uids:
                continue
            seen_material_uids.add(material_uid)
            materials.append(resource)
            slots.append(MaterialSlot(
                slot_name=slot_name,
                material_uid=material_uid,
            ))

    return materials, slots
