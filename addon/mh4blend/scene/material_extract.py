"""Blender material adapter for the manifest material model (B4-mat).

Dag4blend's ``Material.dagormat`` is the authoring source.  The adapter only
reads its public ``shader_class``, ``sides``, ``optional`` and ``textures``
members; the material node tree is deliberately not a second source of truth.
"""

import json

import bpy

from ..core.materials import MaterialValueError
from ..core.model import MaterialResource, MaterialSlot
from ..core.uid import PROP_UID, ensure_uid
from ..core.validate import MHValidationError

__all__ = [
    "DEFAULT_SHADER_CLASS",
    "PROP_IMPORTED_MATERIAL_PAYLOAD",
    "extract_collection_materials",
    "extract_materials_from_objects",
    "normalize_texture_path",
]

DEFAULT_SHADER_CLASS = "rendinst_simple"
PROP_IMPORTED_MATERIAL_PAYLOAD = "mh_material_payload_json"


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


def normalize_texture_path(texture_path):
    """Return the authored dagormat texture reference used by UE.

    Dagormat already owns the texture identity.  Empty slots are omitted,
    Blender ``//`` references are resolved through ``bpy.path.abspath``, and
    every other non-empty reference (absolute or relative, including a path
    written for another platform) is preserved.  No source root is required
    and this adapter never copies or probes texture files.

    """
    if not isinstance(texture_path, str):
        return None
    cleaned = texture_path.strip()
    if not cleaned:
        return None
    if cleaned.startswith("//"):
        if not bpy.data.filepath:
            raise ValueError(
                "Blender-relative texture path requires a saved .blend file")
        return bpy.path.abspath(cleaned)
    return cleaned


def _dagormat_material_resource(material, uid):
    """Return the configured dagormat resource, or ``None`` when empty.

    Keeping this read path separate lets the composite importer verify that a
    best-effort RNA hydration is semantically complete before a non-empty
    dagormat shader becomes authoritative over its lossless JSON fallback.
    """
    dagormat = getattr(material, "dagormat", None)
    shader_class = ""
    if dagormat is not None:
        raw_shader = getattr(dagormat, "shader_class", "")
        if isinstance(raw_shader, str):
            shader_class = raw_shader.strip()
    if not shader_class:
        return None

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
            normalized = normalize_texture_path(raw_path)
        except ValueError as exc:
            raise MHValidationError(
                "MH_E_INVALID_MATERIAL_VALUE", [uid], f"{slot}: {exc}")
        if normalized is not None:
            textures[slot] = normalized

    return MaterialResource(
        uid=uid,
        name=material.name,
        shader_class=shader_class,
        params=params,
        textures=textures,
    )


def _dagormat_disk_payload(material):
    """Read only dagormat and return its normalized payload for verification."""
    uid = ensure_uid(material)
    resource = _dagormat_material_resource(material, uid)
    return resource.disk_payload() if resource is not None else None


def _material_resource(material):
    uid = ensure_uid(material)
    dagormat_resource = _dagormat_material_resource(material, uid)

    # A dependency-aware composite import can run without dag4blend enabled.
    # In that case the importer stores the manifest material row verbatim so a
    # later FBX re-export preserves material identity and payload. A configured
    # real dagormat remains authoritative over this imported fallback.
    if dagormat_resource is None:
        fallback = material.get(PROP_IMPORTED_MATERIAL_PAYLOAD)
        if isinstance(fallback, str) and fallback:
            try:
                row = json.loads(fallback)
                if not isinstance(row, dict):
                    raise ValueError("payload must be a JSON object")
                row_uid = row.get("uid")
                if row_uid is not None and row_uid != uid:
                    raise ValueError(
                        f"payload uid {row_uid!r} does not match {uid!r}")
                fallback_resource = MaterialResource(
                    uid=uid,
                    name=material.name,
                    shader_class=row["shader_class"],
                    params=row.get("params", {}),
                    textures=row.get("textures", {}),
                )
                # Force the same canonical validation used by manifest output.
                fallback_resource.disk_payload()
                return fallback_resource
            except (KeyError, TypeError, ValueError, json.JSONDecodeError,
                    MaterialValueError) as exc:
                raise MHValidationError(
                    "MH_E_INVALID_MATERIAL_VALUE", [uid],
                    f"invalid {PROP_IMPORTED_MATERIAL_PAYLOAD}: {exc}")

        # An unconfigured or unavailable source intentionally becomes a
        # minimal UE placeholder. Do not leak stale dagormat values when its
        # shader_class is empty.
        return MaterialResource(
            uid=uid,
            name=material.name,
            shader_class=DEFAULT_SHADER_CLASS,
            params={},
            textures={},
        )
    return dagormat_resource


def extract_materials_from_objects(objects):
    """Return ``(materials, material_slots)`` for explicit mesh objects.

    The explicit-object form lets the standalone FBX exporter implement
    dag4blend's ``Col.Joined`` semantics (selected collection plus recursive
    children) without creating temporary Blender collections.
    """
    mesh_objects = [obj for obj in objects if obj.type == "MESH"]
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

            resource = _material_resource(material)
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


def extract_collection_materials(collection):
    """Return ``(materials, material_slots)`` for one GEOMETRY resource.

    Mesh objects are ordered by their already-assigned object UID, then by
    Blender slot order.  A MaterialUID contributes at most one table row; its
    first occurrence wins.  Reusing one slot name for different MaterialUIDs
    is ambiguous and therefore blocks the export.
    """
    return extract_materials_from_objects(collection.objects)
