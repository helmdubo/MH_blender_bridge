"""Blender adapter and crash-safe writer for Source Protocol v4 materials."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path

import bpy

from ..core.canonical import validate_resource_name
from ..core.materials import (
    MaterialValueError,
    material_json_bytes,
    parse_material,
    resolve_texture_reference,
)
from ..core.model import MaterialResource
from ..core.payload_publish_v2 import atomic_publish_bytes

__all__ = [
    "PreparedMaterialExport",
    "apply_material_resource",
    "material_class_for_export",
    "prepare_blender_material_export",
    "read_material_file",
    "write_prepared_material",
]


@dataclass(frozen=True)
class PreparedMaterialExport:
    resource: MaterialResource
    target: Path
    payload: bytes


def _resolved_root(source_root) -> Path:
    if not isinstance(source_root, (str, os.PathLike)) or not str(source_root).strip():
        raise ValueError("Configure Project Source Root in the MH addon preferences")
    root = Path(bpy.path.abspath(os.fspath(source_root))).resolve(strict=False)
    if not root.is_dir():
        raise ValueError(f"Project Source Root does not exist: {root}")
    return root


def _inside(root: Path, path: Path) -> bool:
    try:
        return os.path.commonpath([
            os.path.normcase(str(root)), os.path.normcase(str(path)),
        ]) == os.path.normcase(str(root))
    except ValueError:
        return False


def _resolve_material_target(root: Path, output: Path, name: str) -> Path:
    matches = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() != ".material":
            continue
        if path.stem.casefold() != name:
            continue
        if path.suffix != ".material" or path.stem != name:
            raise MaterialValueError(
                "MH_E_NONCANONICAL_RESOURCE_NAME", str(path),
                "material filename must use the exact lowercase logical name "
                "and .material suffix")
        matches.append(path.resolve(strict=False))
    matches.sort(key=lambda path: str(path).replace("\\", "/"))
    if len(matches) > 1:
        raise MaterialValueError(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", name,
            "multiple material resources share this logical name: "
            + ", ".join(str(path) for path in matches))
    return matches[0] if matches else output / f"{name}.material"


def _texture_token(image, path: str) -> str:
    if image is None:
        raise MaterialValueError(
            "MH_E_UNRESOLVED_TEXTURE_REFERENCE", path,
            "texture slot has no Blender image")
    authored_path = str(getattr(image, "filepath", "") or "").strip()
    source = Path(bpy.path.abspath(authored_path)) if authored_path else Path(image.name)
    token = source.stem
    # The codec performs the exact fail-closed token validation.
    parse_material({"class": "probe", "textures": {"tex0": token}})
    return token


def material_class_for_export(material) -> str:
    """Return the authored class token without guessing or repairing it.

    The dedicated v4 override remains authoritative.  When it is empty,
    reuse dag4blend's semantic shader class so artists do not have to enter
    the same token twice.  Dagor's ``None`` sentinel means "not authored";
    every other value is returned verbatim and is still checked by the
    strict v4 codec.
    """
    settings = getattr(material, "mh4blend", None)
    explicit = getattr(settings, "material_class", "")
    if explicit != "":
        return explicit

    dagormat = getattr(material, "dagormat", None)
    authored = getattr(dagormat, "shader_class", "")
    return "" if authored in (None, "", "None") else authored


def _extract_resource(material) -> MaterialResource:
    # Preserve the canonical name diagnostic verbatim; identity is external to
    # the material grammar and must not be reclassified as a codec failure.
    validate_resource_name(material.name)
    if not hasattr(material, "mh4blend"):
        raise MaterialValueError(
            "MH_E_MATERIAL_GRAMMAR", material.name,
            "material has no registered mh4blend property group")
    settings = material.mh4blend
    mode = settings.mode
    if mode == "LIBRARY":
        if settings.twosided_override or settings.textures or settings.params:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", material.name,
                "library material cannot contain local overrides")
        return MaterialResource(name=material.name, library=settings.library)
    if mode != "CLASS":
        raise MaterialValueError(
            "MH_E_MATERIAL_GRAMMAR", "mode", "must be CLASS or LIBRARY")

    textures = {}
    for row in settings.textures:
        slot = f"tex{row.slot}"
        if slot in textures:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", f"textures.{slot}",
                "duplicate texture slot")
        textures[slot] = _texture_token(row.image, f"textures.{slot}")

    params = {}
    for row in settings.params:
        if row.name in params:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", f"params.{row.name}",
                "duplicate parameter")
        params[row.name] = (
            row.scalar if row.kind == "SCALAR" else list(row.vector))

    return MaterialResource(
        name=material.name,
        material_class=material_class_for_export(material),
        twosided=settings.twosided if settings.twosided_override else None,
        textures=textures,
        params=params,
    )


def prepare_blender_material_export(
        material, output_dir, *, source_root) -> PreparedMaterialExport:
    """Extract and fully validate one Blender material without writing files."""
    if material is None:
        raise ValueError("material is required")
    root = _resolved_root(source_root)
    output = Path(bpy.path.abspath(os.fspath(output_dir))).resolve(strict=False)
    if not _inside(root, output):
        raise ValueError("Material output folder must be inside Project Source Root")

    try:
        resource = _extract_resource(material)
        payload = material_json_bytes(resource)
        for token in resource.textures.values():
            resolve_texture_reference(root, token)
    except MaterialValueError as exc:
        raise MaterialValueError(
            exc.code,
            f"material {material.name!r} / {exc.path}",
            exc.message,
        ) from exc

    target = _resolve_material_target(root, output, resource.name)
    if target.exists() and target.is_dir():
        raise ValueError(f"Material target exists as a directory: {target}")
    return PreparedMaterialExport(resource=resource, target=target, payload=payload)


def _validate_read_back(prepared: PreparedMaterialExport, payload: bytes) -> None:
    decoded = parse_material(payload, name=prepared.resource.name)
    if material_json_bytes(decoded) != prepared.payload:
        raise RuntimeError(
            "MH_E_MATERIAL_GRAMMAR: staged material failed canonical read-back")


def write_prepared_material(
        prepared: PreparedMaterialExport, *, source_root) -> dict:
    """Always publish the complete file via sibling tmp/read-back/replace."""
    if not isinstance(prepared, PreparedMaterialExport):
        raise TypeError("prepared must be PreparedMaterialExport")
    root = _resolved_root(source_root)
    target = prepared.target.resolve(strict=False)
    if not _inside(root, target):
        raise ValueError("Material target must be inside Project Source Root")
    if target.exists() and target.is_dir():
        raise ValueError(f"Material target exists as a directory: {target}")

    receipt = atomic_publish_bytes(
        target,
        prepared.payload,
        source_root=root,
        read_back_validator=lambda payload: _validate_read_back(prepared, payload),
    )
    return {
        "ok": True,
        "filepath": str(target),
        "resource_name": prepared.resource.name,
        "bytes": receipt["bytes"],
        "written": True,
    }


def read_material_file(filepath) -> MaterialResource:
    """Read one v4 material file; filename remains the resource identity."""
    path = Path(bpy.path.abspath(os.fspath(filepath))).resolve(strict=True)
    if path.suffix != ".material":
        raise ValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME: material filename must end "
            "in lowercase .material")
    validate_resource_name(path.stem)
    return parse_material(path.read_bytes(), name=path.stem)


def apply_material_resource(material, resource: MaterialResource, *, source_root) -> None:
    """Populate the dedicated mh4blend property group from one strict DTO."""
    # Validate before mutating Blender state.
    payload = material_json_bytes(resource)
    resource = parse_material(payload, name=resource.name)
    texture_paths = {
        slot: resolve_texture_reference(source_root, token)
        for slot, token in resource.textures.items()
    }
    texture_images = {
        slot: bpy.data.images.load(str(path), check_existing=True)
        for slot, path in texture_paths.items()
    }

    settings = material.mh4blend
    settings.textures.clear()
    settings.params.clear()
    if resource.library is not None:
        settings.mode = "LIBRARY"
        settings.library = resource.library
        settings.material_class = ""
        settings.twosided_override = False
        return

    settings.mode = "CLASS"
    settings.material_class = resource.material_class
    settings.library = ""
    settings.twosided_override = resource.twosided is not None
    if resource.twosided is not None:
        settings.twosided = resource.twosided
    for slot, image in texture_images.items():
        row = settings.textures.add()
        row.slot = int(slot[3:])
        row.image = image
    for name, value in resource.params.items():
        row = settings.params.add()
        row.name = name
        if isinstance(value, list):
            row.kind = "VECTOR"
            row.vector = value
        else:
            row.kind = "SCALAR"
            row.scalar = value
