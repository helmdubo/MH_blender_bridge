"""Pure material fingerprinting and optional shader-registry parsing.

Material identity (kind + logical name) is deliberately outside the fingerprint. The
content hash answers only whether the UE material payload changed:
``shader_class + params + textures``.  Numeric material parameters use the
same p=6 generic/property quantization as the canonical JSON contract, so an
integer and the equivalent float cannot create a phantom diff.

The registry is optional.  Callers represent an absent registry as ``None``;
that disables shader-class checking rather than treating absence as invalid.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import math
from typing import Any

from .canonical import P_PROPERTIES, canonical_json_bytes, hash_hex, nfc, quantize

__all__ = [
    "MaterialRegistry",
    "MaterialValueError",
    "RegistryValidationError",
    "material_canonical_form",
    "material_content_hash",
    "material_disk_payload",
    "parse_registry",
]


class RegistryValidationError(ValueError):
    """Configured registry content is not the supported ``mh.registry`` v1."""


class MaterialValueError(ValueError):
    """Material payload cannot be represented by the manifest contract."""

    def __init__(self, code: str, path: str, message: str):
        self.code = code
        self.path = path
        super().__init__(f"{code}: {path}: {message}")


@dataclass(frozen=True)
class MaterialRegistry:
    """Validated registry subset needed by the Blender material exporter."""

    shader_classes: frozenset[str]

    def contains(self, shader_class: str) -> bool:
        return nfc(shader_class) in self.shader_classes


def _disk_number(q: int) -> int | float:
    """Render one p=6 integer in the human-facing manifest representation."""
    scale = 10 ** P_PROPERTIES
    if q % scale == 0:
        return q // scale
    return q / scale


def _material_value(value: Any, path: str) -> tuple[Any, Any]:
    """Return ``(normalized_disk_value, canonical_value)`` for one value."""
    if value is None or isinstance(value, bool):
        return value, value
    if isinstance(value, str):
        normalized = nfc(value)
        return normalized, normalized
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        try:
            finite = math.isfinite(float(value))
        except (OverflowError, TypeError, ValueError) as exc:
            raise MaterialValueError(
                "MH_E_INVALID_MATERIAL_VALUE", path, str(exc)) from exc
        if not finite:
            raise MaterialValueError(
                "MH_E_NAN_INF_VALUE", path, "NaN/Inf is not a valid material value")
        try:
            q = quantize(value, P_PROPERTIES)
        except (TypeError, ValueError, OverflowError) as exc:
            raise MaterialValueError(
                "MH_E_INVALID_MATERIAL_VALUE", path, str(exc)) from exc
        return _disk_number(q), q
    if isinstance(value, (list, tuple)):
        disk_items = []
        canonical_items = []
        for index, item in enumerate(value):
            disk_item, canonical_item = _material_value(
                item, f"{path}[{index}]")
            disk_items.append(disk_item)
            canonical_items.append(canonical_item)
        return disk_items, canonical_items
    if isinstance(value, dict):
        disk_out: dict[str, Any] = {}
        canonical_out: dict[str, Any] = {}
        source_keys: dict[str, str] = {}
        for key, item in value.items():
            if not isinstance(key, str):
                raise MaterialValueError(
                    "MH_E_INVALID_MATERIAL_VALUE", path,
                    f"object keys must be strings, got {type(key).__name__}"
                )
            normalized = nfc(key)
            if normalized in disk_out:
                raise MaterialValueError(
                    "MH_E_INVALID_MATERIAL_VALUE", path,
                    "keys collide after NFC normalization: "
                    f"{source_keys[normalized]!r} and {key!r}",
                )
            source_keys[normalized] = key
            disk_item, canonical_item = _material_value(
                item, f"{path}.{normalized}")
            disk_out[normalized] = disk_item
            canonical_out[normalized] = canonical_item
        return disk_out, canonical_out
    raise MaterialValueError(
        "MH_E_INVALID_MATERIAL_VALUE", path,
        f"value of type {type(value).__name__} is not JSON-compatible",
    )


def _normalized_material(
    shader_class: str, params: dict, textures: dict,
) -> tuple[dict, dict]:
    if not isinstance(shader_class, str):
        raise MaterialValueError(
            "MH_E_INVALID_MATERIAL_VALUE", "shader_class", "must be a string")
    if not isinstance(params, dict):
        raise MaterialValueError(
            "MH_E_INVALID_MATERIAL_VALUE", "params", "must be an object")
    if not isinstance(textures, dict):
        raise MaterialValueError(
            "MH_E_INVALID_MATERIAL_VALUE", "textures", "must be an object")

    normalized_shader = nfc(shader_class)
    disk_params, canonical_params = _material_value(params, "params")
    disk_textures, canonical_textures = _material_value(textures, "textures")
    disk = {
        "shader_class": normalized_shader,
        "params": disk_params,
        "textures": disk_textures,
    }
    canonical = {
        "shader_class": normalized_shader,
        "params": canonical_params,
        "textures": canonical_textures,
    }
    return disk, canonical


def material_disk_payload(shader_class: str, params: dict, textures: dict) -> dict:
    """Return normalized material semantics for the self-contained payload."""
    disk, _canonical = _normalized_material(shader_class, params, textures)
    return disk


def material_canonical_form(shader_class: str, params: dict, textures: dict) -> dict:
    """Return the canonical value tree used for a material content hash."""
    _disk, canonical = _normalized_material(shader_class, params, textures)
    return canonical


def material_content_hash(shader_class: str, params: dict, textures: dict) -> str:
    """XXH3-64 fingerprint of ``shader_class + params + textures``."""
    canonical = material_canonical_form(shader_class, params, textures)
    return hash_hex(canonical_json_bytes(canonical))


def parse_registry(value: MaterialRegistry | dict | str | bytes) -> MaterialRegistry:
    """Parse and validate the supported subset of ``mh.registry`` v1.

    Extra top-level fields are tolerated because the UE-generated registry may
    grow sections unrelated to Blender material validation.  The three fields
    in the minimal contract are required and validated strictly.
    """
    if isinstance(value, MaterialRegistry):
        return value

    document: Any = value
    if isinstance(value, (str, bytes)):
        try:
            document = json.loads(value)
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            raise RegistryValidationError(f"invalid registry JSON: {exc}") from exc

    if not isinstance(document, dict):
        raise RegistryValidationError("registry must be a JSON object")
    if document.get("schema") != "mh.registry":
        raise RegistryValidationError("registry schema must be 'mh.registry'")
    version = document.get("schema_version")
    if not isinstance(version, int) or isinstance(version, bool) or version != 1:
        raise RegistryValidationError("registry schema_version must be integer 1")
    shader_classes = document.get("shader_classes")
    if not isinstance(shader_classes, list):
        raise RegistryValidationError("registry shader_classes must be an array")

    normalized: set[str] = set()
    for index, shader_class in enumerate(shader_classes):
        if not isinstance(shader_class, str):
            raise RegistryValidationError(
                f"registry shader_classes[{index}] must be a string"
            )
        normalized.add(nfc(shader_class))
    return MaterialRegistry(frozenset(normalized))
