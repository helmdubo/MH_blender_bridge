"""Production codec and Blender-parser reader for FBX Carrier B passports."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import math
import os
import re
import unicodedata
import uuid

from .canonical import validate_resource_name

__all__ = [
    "PASSPORT_PROPERTY",
    "PASSPORT_SCHEMA",
    "PASSPORT_SCHEMA_VERSION",
    "FbxPassportRead",
    "canonical_passport",
    "descriptor_hash",
    "make_fbx_passport",
    "parse_passport_text",
    "read_fbx_passport",
]


PASSPORT_PROPERTY = "mh_fbx_passport"
PASSPORT_SCHEMA = "mh.fbx_passport"
PASSPORT_SCHEMA_VERSION = 1

_PASSPORT_FIELDS = frozenset({
    "schema", "schema_version", "resource_uid", "kind", "name",
    "lod_levels", "lod_policy", "geometry_hash", "material_slots",
    "properties", "exporter",
})
_SLOT_FIELDS = frozenset({
    "slot_name", "material_uid", "material_name_hint",
})
_HASH_RE = re.compile(r"^xxh3:[0-9a-f]{16}$")


@dataclass(frozen=True)
class FbxPassportRead:
    document: dict
    canonical_text: str
    descriptor_hash: str
    copy_count: int
    model_names: tuple[str, ...]


def _fail(message: str):
    raise ValueError(f"MH_E_PASSPORT_INVALID: {message}")


def _nfc(value):
    if isinstance(value, str):
        return unicodedata.normalize("NFC", value)
    if isinstance(value, list):
        return [_nfc(item) for item in value]
    if isinstance(value, dict):
        normalized = {_nfc(key): _nfc(item) for key, item in value.items()}
        if len(normalized) != len(value):
            _fail("keys collide after Unicode NFC")
        return normalized
    return value


def _json_value(value, path):
    """Validate one JSON bag without Python's tuple/key coercions."""
    if value is None or isinstance(value, (bool, int, str)):
        return _nfc(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            _fail(f"{path} contains NaN or infinity")
        return value
    if isinstance(value, list):
        return [_json_value(item, f"{path}[]") for item in value]
    if isinstance(value, dict):
        normalized = {}
        for key, item in value.items():
            if not isinstance(key, str):
                _fail(f"{path} keys must be strings")
            normalized_key = unicodedata.normalize("NFC", key)
            if normalized_key in normalized:
                _fail(f"{path} keys collide after Unicode NFC")
            normalized[normalized_key] = _json_value(
                item, f"{path}.{normalized_key}")
        return normalized
    _fail(f"{path} contains non-JSON {type(value).__name__}")


def canonical_passport(document: dict) -> str:
    """Validate and serialize the exact one-line passport representation."""
    normalized = _validated_document(document)
    try:
        text = json.dumps(
            _nfc(normalized), ensure_ascii=False, allow_nan=False,
            sort_keys=True, separators=(",", ":"),
        )
    except (TypeError, ValueError) as exc:
        _fail(f"passport is not canonical JSON: {exc}")
    if "\n" in text or "\r" in text:
        _fail("passport must occupy one custom-property line")
    return text


def _canonical_uuid(value, field):
    if not isinstance(value, str):
        _fail(f"{field} must be a canonical UUID string")
    try:
        canonical = str(uuid.UUID(value))
    except (ValueError, AttributeError, TypeError) as exc:
        _fail(f"{field}: {exc}")
    if value != canonical:
        _fail(f"{field} must use canonical lowercase UUID spelling")
    return value


def _validated_document(document):
    if not isinstance(document, dict):
        _fail("root must be an object")
    fields = set(document)
    if fields != _PASSPORT_FIELDS:
        _fail(
            "passport fields differ; missing="
            f"{sorted(_PASSPORT_FIELDS - fields)}, "
            f"unknown={sorted(fields - _PASSPORT_FIELDS)}")
    if document["schema"] != PASSPORT_SCHEMA:
        _fail("unknown schema")
    if (isinstance(document["schema_version"], bool)
            or not isinstance(document["schema_version"], int)
            or document["schema_version"] != PASSPORT_SCHEMA_VERSION):
        _fail("unsupported schema_version")
    resource_uid = _canonical_uuid(document["resource_uid"], "resource_uid")
    if document["kind"] != "static_mesh":
        _fail("kind must be static_mesh")
    try:
        validate_resource_name(document["name"])
    except (TypeError, ValueError) as exc:
        _fail(f"name: {exc}")

    lod_levels = document["lod_levels"]
    if (not isinstance(lod_levels, list) or not lod_levels
            or any(isinstance(level, bool) or not isinstance(level, int)
                   or level < 0 for level in lod_levels)
            or lod_levels != list(range(len(lod_levels)))):
        _fail("lod_levels must be contiguous [0..N]")
    lod_policy = document["lod_policy"]
    if lod_policy not in {"authored", "generated", "nanite"}:
        _fail("lod_policy must be authored, generated or nanite")
    if len(lod_levels) > 1 and lod_policy != "authored":
        _fail("multiple lod_levels require authored lod_policy")
    geometry_hash = document["geometry_hash"]
    if not isinstance(geometry_hash, str) or not _HASH_RE.fullmatch(
            geometry_hash):
        _fail("geometry_hash must be xxh3 plus 16 lowercase hex digits")

    slots = document["material_slots"]
    if not isinstance(slots, list):
        _fail("material_slots must be an array")
    slot_names = set()
    normalized_slots = []
    for index, slot in enumerate(slots):
        if not isinstance(slot, dict) or set(slot) != _SLOT_FIELDS:
            _fail(f"material_slots[{index}] has invalid fields")
        slot_name = slot["slot_name"]
        hint = slot["material_name_hint"]
        if not isinstance(slot_name, str) or not slot_name:
            _fail(f"material_slots[{index}].slot_name must be non-empty")
        if not isinstance(hint, str):
            _fail(f"material_slots[{index}].material_name_hint must be text")
        slot_name = unicodedata.normalize("NFC", slot_name)
        hint = unicodedata.normalize("NFC", hint)
        material_uid = _canonical_uuid(
            slot["material_uid"], f"material_slots[{index}].material_uid")
        if slot_name in slot_names:
            _fail(f"duplicate material slot_name {slot_name!r}")
        slot_names.add(slot_name)
        normalized_slots.append({
            "slot_name": slot_name,
            "material_uid": material_uid,
            "material_name_hint": hint,
        })
    if [slot["slot_name"] for slot in normalized_slots] != sorted(
            slot["slot_name"] for slot in normalized_slots):
        _fail("material_slots must be sorted by slot_name")

    if not isinstance(document["properties"], dict):
        _fail("properties must be an object")
    properties = _json_value(document["properties"], "properties")
    exporter = document["exporter"]
    if not isinstance(exporter, str) or not exporter:
        _fail("exporter must be a non-empty string")
    exporter = unicodedata.normalize("NFC", exporter)

    return {
        "schema": PASSPORT_SCHEMA,
        "schema_version": PASSPORT_SCHEMA_VERSION,
        "resource_uid": resource_uid,
        "kind": "static_mesh",
        "name": document["name"],
        "lod_levels": list(lod_levels),
        "lod_policy": lod_policy,
        "geometry_hash": geometry_hash,
        "material_slots": normalized_slots,
        "properties": properties,
        "exporter": exporter,
    }


def make_fbx_passport(
        *, resource_uid: str, name: str, lod_levels, lod_policy: str,
        geometry_hash: str, material_slots, properties, exporter: str) -> dict:
    """Build and validate the exact production passport document."""
    return _validated_document({
        "schema": PASSPORT_SCHEMA,
        "schema_version": PASSPORT_SCHEMA_VERSION,
        "resource_uid": resource_uid,
        "kind": "static_mesh",
        "name": name,
        "lod_levels": list(lod_levels),
        "lod_policy": lod_policy,
        "geometry_hash": geometry_hash,
        "material_slots": list(material_slots),
        "properties": properties,
        "exporter": exporter,
    })


def parse_passport_text(text: str) -> dict:
    if not isinstance(text, str):
        _fail("carrier value must be text")
    try:
        document = json.loads(text)
    except (json.JSONDecodeError, UnicodeError) as exc:
        _fail(f"invalid JSON: {exc}")
    canonical = canonical_passport(document)
    if text != canonical:
        _fail("carrier value is not canonical one-line JSON")
    return _validated_document(document)


def descriptor_hash(document: dict) -> str:
    """Hash metadata only; geometry_hash has its independent write trigger."""
    normalized = _validated_document(document)
    normalized.pop("geometry_hash")
    try:
        data = json.dumps(
            _nfc(normalized), ensure_ascii=False, allow_nan=False,
            sort_keys=True, separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        _fail(f"descriptor is not canonical JSON: {exc}")
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _default_fbx_parser(path):
    try:
        from io_scene_fbx import parse_fbx
    except ImportError as exc:
        _fail(f"Blender FBX parser is unavailable: {exc}")
    try:
        root, _version = parse_fbx.parse(os.fspath(path))
    except Exception as exc:
        _fail(f"FBX parser rejected payload: {exc}")
    return root, parse_fbx.data_types.STRING


def _first_child(element, element_id):
    for child in element.elems:
        if child.id == element_id:
            return child
    return None


def _model_name(model):
    if len(model.props) > 1 and isinstance(model.props[1], bytes):
        raw = model.props[1].split(b"\x00", 1)[0]
        return raw.decode("utf-8", "replace")
    return "<unnamed Model>"


def _carrier_from_model(model, string_type):
    props70 = _first_child(model, b"Properties70")
    matches = []
    if props70 is not None:
        for prop in props70.elems:
            if prop.id != b"P" or not prop.props:
                continue
            if prop.props[0] == PASSPORT_PROPERTY.encode("utf-8"):
                matches.append(prop)
    name = _model_name(model)
    if len(matches) != 1:
        _fail(
            f"MESH Model {name!r} must contain exactly one "
            f"{PASSPORT_PROPERTY}; found {len(matches)}")
    prop = matches[0]
    if (len(prop.props) < 5 or len(prop.props_type) < 5
            or prop.props[1] != b"KString" or b"U" not in prop.props[3]
            or prop.props_type[4] != string_type
            or not isinstance(prop.props[4], bytes)):
        _fail(f"MESH Model {name!r} has malformed passport property")
    try:
        return name, prop.props[4].decode("utf-8", "strict")
    except UnicodeDecodeError as exc:
        _fail(f"MESH Model {name!r} passport is not UTF-8: {exc}")


def read_fbx_passport(path, *, parser=None) -> FbxPassportRead:
    """Read every MESH Model carrier and require byte-identical consensus."""
    root, string_type = (parser or _default_fbx_parser)(path)
    objects = _first_child(root, b"Objects")
    if objects is None:
        _fail("FBX has no Objects section")
    models = [
        element for element in objects.elems
        if (element.id == b"Model" and len(element.props) >= 3
            and element.props[2] == b"Mesh")
    ]
    if not models:
        _fail("FBX has no MESH Models")
    copies = [_carrier_from_model(model, string_type) for model in models]
    texts = {text for _name, text in copies}
    if len(texts) != 1:
        _fail("Carrier B copies differ byte-for-byte across MESH Models")
    text = copies[0][1]
    document = parse_passport_text(text)
    return FbxPassportRead(
        document=document,
        canonical_text=text,
        descriptor_hash=descriptor_hash(document),
        copy_count=len(copies),
        model_names=tuple(name for name, _text in copies),
    )
