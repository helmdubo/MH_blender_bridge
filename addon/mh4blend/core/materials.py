"""Pure Source Protocol v4 material codec.

The disk payload has no identity, schema, version, or compatibility fields.
Identity comes from the ``.material`` filename; this module only validates and
serializes the closed grammar from docs/08 section 5.
"""

from __future__ import annotations

import math
import os
from pathlib import Path
import re
from typing import Any

from .model import MaterialResource
from .canonical_json import (
    CanonicalJSONDuplicateKey,
    CanonicalJSONNonFinite,
    CanonicalJSONSyntaxError,
    canonical_json_bytes,
    narrow_float32,
    parse_json,
)

__all__ = [
    "MATERIAL_TEXTURE_EXTENSIONS",
    "MaterialValueError",
    "material_document",
    "material_json_bytes",
    "parse_material",
    "resolve_texture_reference",
]


MATERIAL_TEXTURE_EXTENSIONS = frozenset({
    ".png", ".tga", ".tif", ".tiff", ".exr", ".jpg", ".jpeg", ".dds",
    ".hdr",
})

_TOKEN_RE = re.compile(r"^[a-z0-9_]+$")
_TEXTURE_SLOT_RE = re.compile(r"^tex(?:[0-9]|1[0-5])$")
_CLASS_FIELDS = frozenset({"class", "twosided", "textures", "params"})


class MaterialValueError(ValueError):
    """A material cannot be represented without losing v4 semantics."""

    def __init__(self, code: str, path: str, message: str):
        self.code = code
        self.path = path
        self.message = message
        super().__init__(f"{code}: {path}: {message}")


def _error(path: str, message: str) -> MaterialValueError:
    return MaterialValueError("MH_E_MATERIAL_GRAMMAR", path, message)


def _token(value: Any, path: str, *, texture: bool = False) -> str:
    if not isinstance(value, str) or _TOKEN_RE.fullmatch(value) is None:
        code = (
            "MH_E_NONCANONICAL_TEXTURE_REFERENCE"
            if texture else "MH_E_MATERIAL_GRAMMAR"
        )
        raise MaterialValueError(
            code, path,
            f"value {value!r} must match [a-z0-9_]+ exactly; "
            "no repair is performed")
    return value


def _number(value: Any, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _error(path, "must be a number")
    try:
        finite = math.isfinite(value)
    except (OverflowError, TypeError, ValueError) as exc:
        raise _error(path, str(exc)) from exc
    if not finite:
        raise MaterialValueError(
            "MH_E_NAN_INF_VALUE", path, "NaN/Inf is not a material value")
    try:
        narrowed = narrow_float32(value)
    except (TypeError, ValueError) as exc:
        raise _error(path, "must be representable as finite IEEE float32") from exc
    return narrowed


def _textures(value: Any) -> dict[str, str]:
    if not isinstance(value, dict):
        raise _error("textures", "must be an object")
    out: dict[str, str] = {}
    for key, reference in value.items():
        if not isinstance(key, str) or _TEXTURE_SLOT_RE.fullmatch(key) is None:
            raise _error(
                "textures", "keys must be tex0 through tex15 without leading zeroes")
        out[key] = _token(reference, f"textures.{key}", texture=True)
    return dict(sorted(out.items(), key=lambda item: int(item[0][3:])))


def _params(value: Any) -> dict[str, float | list[float] | str | bool]:
    if not isinstance(value, dict):
        raise _error("params", "must be an object")
    out: dict[str, float | list[float] | str | bool] = {}
    for key, parameter in value.items():
        _token(key, "params key")
        path = f"params.{key}"
        if isinstance(parameter, (str, bool)):
            out[key] = parameter
        elif isinstance(parameter, (list, tuple)):
            if len(parameter) != 4:
                raise _error(path, "vector parameters must contain exactly 4 numbers")
            out[key] = [
                _number(component, f"{path}[{index}]")
                for index, component in enumerate(parameter)
            ]
        else:
            out[key] = _number(parameter, path)
    return dict(sorted(out.items()))


def material_document(material: MaterialResource) -> dict[str, Any]:
    """Validate one DTO and return its canonical insertion-ordered document."""
    if not isinstance(material, MaterialResource):
        raise TypeError("material must be MaterialResource")
    has_class = material.material_class is not None
    has_library = material.library is not None
    if has_class == has_library:
        raise _error("$", "exactly one of class or library is required")

    if has_library:
        library = _token(material.library, "library")
        if material.twosided is not None or material.textures or material.params:
            raise _error(
                "$", "library form contains exactly one field and no overrides")
        return {"library": library}

    document: dict[str, Any] = {
        "class": _token(material.material_class, "class"),
    }
    if material.twosided is not None:
        if not isinstance(material.twosided, bool):
            raise _error("twosided", "must be a bool")
        document["twosided"] = material.twosided
    if material.textures:
        document["textures"] = _textures(material.textures)
    if material.params:
        document["params"] = _params(material.params)
    return document


def material_json_bytes(material: MaterialResource | dict[str, Any]) -> bytes:
    """Return the canonical UTF-8/LF byte form required by docs/08 section 5."""
    if isinstance(material, dict):
        material = parse_material(material)
    document = material_document(material)
    return canonical_json_bytes(document)


def _parse_json(value: str | bytes) -> Any:
    try:
        return parse_json(value)
    except CanonicalJSONDuplicateKey as exc:
        raise _error(exc.key, "duplicate JSON field") from exc
    except CanonicalJSONNonFinite as exc:
        raise MaterialValueError(
            "MH_E_NAN_INF_VALUE", "$", f"invalid number {exc.token}") from exc
    except CanonicalJSONSyntaxError as exc:
        raise _error("$", f"invalid JSON: {exc}") from exc


def parse_material(
        value: dict[str, Any] | str | bytes, *, name: str = "") -> MaterialResource:
    """Parse the closed disk grammar without normalization or legacy fallback."""
    document = _parse_json(value) if isinstance(value, (str, bytes)) else value
    if not isinstance(document, dict):
        raise _error("$", "material payload must be an object")

    fields = set(document)
    if fields == {"library"}:
        material = MaterialResource(name=name, library=document["library"])
        # Run through the writer validator so reader and writer share one truth.
        material_document(material)
        return material

    unknown = fields - _CLASS_FIELDS
    if unknown:
        raise _error("$", f"unknown field(s): {', '.join(sorted(unknown))}")
    if "class" not in document:
        raise _error("$", "class form requires field 'class'")

    twosided = document.get("twosided")
    if "twosided" in document and not isinstance(twosided, bool):
        raise _error("twosided", "must be a bool")
    material = MaterialResource(
        name=name,
        material_class=document["class"],
        twosided=twosided if "twosided" in document else None,
        textures=_textures(document["textures"]) if "textures" in document else {},
        params=_params(document["params"]) if "params" in document else {},
    )
    material_document(material)
    return material


def resolve_texture_reference(
        source_root: str | os.PathLike, token: str) -> Path:
    """Resolve one extensionless texture ResourceKey by direct source scan."""
    _token(token, "texture", texture=True)
    root = Path(source_root).resolve(strict=False)
    if not root.is_dir():
        raise ValueError(f"Project Source Root does not exist: {root}")
    matches = []
    for path in root.rglob("*"):
        if not path.is_file() or path.stem.casefold() != token:
            continue
        physical = path.resolve(strict=True)
        try:
            inside = os.path.commonpath([
                os.path.normcase(str(root)),
                os.path.normcase(str(physical)),
            ]) == os.path.normcase(str(root))
        except ValueError:
            inside = False
        if not inside:
            raise MaterialValueError(
                "MH_E_TEXTURE_OUTSIDE_ROOT", str(path),
                f"texture resolves outside Project Source Root: {physical}")
        suffix = path.suffix
        if suffix.lower() not in MATERIAL_TEXTURE_EXTENSIONS:
            continue
        if path.stem != token or suffix not in MATERIAL_TEXTURE_EXTENSIONS:
            raise MaterialValueError(
                "MH_E_NONCANONICAL_RESOURCE_NAME", str(path),
                "texture filename must use the exact lowercase logical name "
                "and extension")
        matches.append(path)
    matches.sort(key=lambda path: str(path).replace("\\", "/"))
    if not matches:
        raise MaterialValueError(
            "MH_E_UNRESOLVED_TEXTURE_REFERENCE", token,
            "no texture resource with this logical name exists in source_root")
    if len(matches) != 1:
        rendered = ", ".join(str(path) for path in matches)
        raise MaterialValueError(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", token,
            f"multiple texture resources share this logical name: {rendered}")
    return matches[0]
