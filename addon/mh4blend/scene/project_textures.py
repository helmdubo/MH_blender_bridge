"""Blender adapter for copying and remapping all authored Dagor textures."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import re

import bpy

from ..core.mesh_nodes import strip_blender_duplicate_suffix
from ..core.dagor_names import project_dagor_resource_name
from ..core.project_textures import (
    ProjectTextureError,
    TextureCopyPlan,
    atomic_copy_texture_plans,
    plan_project_texture,
    validate_texture_plans,
)
from ..core.proxymat import read_proxymat
from .resource_markers import COLLECTION_KIND_KEY, COLLECTION_RESOURCE_KEY

__all__ = [
    "DagorTextureBinding",
    "collect_dagor_texture_bindings",
    "copy_all_dagor_textures_to_project",
    "remap_all_dagor_textures_to_project",
]


@dataclass(frozen=True)
class DagorTextureBinding:
    material: object
    material_name: str
    slot: str
    authored_path: str
    transport_suffix: str
    plan: TextureCopyPlan
    writable_textures: object = None
    macro_asset_name: str | None = None


_LOD_RESOURCE_RE = re.compile(
    r"^(?P<base>.+)\.lod(?:s|\d{2})(?:\.\d{3})?$")


def _project_root(source_root) -> Path:
    if not isinstance(source_root, (str, os.PathLike)) or not str(source_root).strip():
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", repr(source_root),
            "Configure Project Source Root in the MH addon preferences")
    root = Path(bpy.path.abspath(os.fspath(source_root))).resolve(strict=False)
    if not root.is_dir():
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", str(root),
            "Project Source Root does not exist")
    return root


def _split_transport_suffix(path: str) -> tuple[str, str]:
    source_path, marker, suffix = path.partition("*?")
    return source_path, marker + suffix if marker else ""


def _loaded_image_texture_source_index() -> dict[str, list[Path]]:
    """Index dag4blend's loaded image carriers once for the whole batch."""
    matches: dict[str, dict[str, Path]] = {}
    for image in bpy.data.images:
        authored_image_path = str(getattr(image, "filepath", "") or "")
        if not authored_image_path:
            continue
        absolute = Path(
            bpy.path.abspath(authored_image_path)).resolve(strict=False)
        if not absolute.is_file():
            continue
        token = absolute.stem.casefold()
        key = os.path.normcase(str(absolute)).casefold()
        matches.setdefault(token, {})[key] = absolute
    return {
        token: sorted(paths.values(), key=lambda path: str(path).casefold())
        for token, paths in matches.items()
    }


def _resolve_dagor_texture_source(
        source_path: str, image_sources: dict[str, list[Path]]) -> str:
    authored = Path(bpy.path.abspath(source_path)).resolve(strict=False)
    if authored.is_file():
        return str(authored)
    filename = source_path.replace("\\", "/").rsplit("/", 1)[-1]
    parsed = Path(filename)
    token = parsed.stem if parsed.suffix else parsed.name
    candidates = image_sources.get(token.casefold(), [])
    if len(candidates) == 1:
        return str(candidates[0])
    if len(candidates) > 1:
        raise ProjectTextureError(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", source_path,
            "Dagor texture basename resolves to multiple loaded image files: "
            + ", ".join(str(path) for path in candidates))
    raise ProjectTextureError(
        "MH_E_INVALID_RESOURCE_SOURCE", source_path,
        "authored Dagor texture path does not exist and its basename has no "
        "resolved loaded image; run dag4blend Find missing textures for all "
        "materials")


def _is_proxy_dagormat(dagormat) -> bool:
    if dagormat is None:
        return False
    if getattr(dagormat, "is_proxy", False) is True:
        return True
    shader_class = str(getattr(dagormat, "shader_class", "") or "")
    return shader_class.endswith(":proxymat")


def _proxy_source(material, dagormat):
    directory = str(getattr(dagormat, "proxy_path", "") or "")
    source = Path(bpy.path.abspath(directory)).resolve(strict=False)
    source = source / (
        strip_blender_duplicate_suffix(str(material.name)) + ".proxymat.blk")
    try:
        return source, read_proxymat(source)
    except (OSError, UnicodeError) as exc:
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", str(source),
            "proxymat source cannot be read; check proxy_path or run the "
            "dag4blend proxymat search") from exc


def _collection_asset_name(collection) -> str | None:
    if collection.get(COLLECTION_KIND_KEY) == "mesh":
        stamped = collection.get(COLLECTION_RESOURCE_KEY)
        if isinstance(stamped, str) and stamped:
            return stamped
    match = _LOD_RESOURCE_RE.fullmatch(
        strip_blender_duplicate_suffix(str(collection.name)))
    if match is None:
        return None
    return project_dagor_resource_name(match.group("base"))


def _object_asset_name(obj) -> str | None:
    candidates = {
        name
        for collection in obj.users_collection
        for name in (_collection_asset_name(collection),)
        if name is not None
    }
    if not candidates:
        match = _LOD_RESOURCE_RE.fullmatch(
            strip_blender_duplicate_suffix(str(obj.name)))
        if match is not None:
            candidates.add(project_dagor_resource_name(match.group("base")))
    if len(candidates) > 1:
        raise ProjectTextureError(
            "MH_E_AMBIGUOUS_RESOURCE_NAME", obj.name,
            "mesh object belongs to multiple Dagor asset definitions: "
            + ", ".join(sorted(candidates)))
    return next(iter(candidates), None)


def _material_macro_asset_names(material) -> tuple[str, ...]:
    names = set()
    for obj in bpy.data.objects:
        if obj.type != "MESH":
            continue
        if not any(slot.material == material for slot in obj.material_slots):
            continue
        asset_name = _object_asset_name(obj)
        if asset_name is None:
            raise ProjectTextureError(
                "MH_E_INVALID_RESOURCE_SOURCE", obj.name,
                f"cannot derive $(ASSET_NAME) for proxymat material "
                f"{material.name!r}; put the object in a .lodNN mesh "
                "collection or use a managed MH mesh collection")
        names.add(asset_name)
    if not names:
        raise ProjectTextureError(
            "MH_E_INVALID_RESOURCE_SOURCE", material.name,
            "proxymat uses $(ASSET_NAME) but no mesh object using this "
            "material was found")
    return tuple(sorted(names))


def _proxymat_texture_path(source: Path, value: str) -> str:
    authored = Path(value)
    if value.startswith("//"):
        path = Path(bpy.path.abspath(value)).resolve(strict=False)
    elif authored.is_absolute():
        path = authored.resolve(strict=False)
    else:
        path = (source.parent / authored).resolve(strict=False)
    return str(path)


def collect_dagor_texture_bindings(
        materials, *, source_root) -> list[DagorTextureBinding]:
    """Preflight every non-empty tex0..tex15 path in deterministic order."""
    root = _project_root(source_root)
    image_sources = _loaded_image_texture_source_index()
    bindings = []
    for material in sorted(materials, key=lambda value: value.name):
        dagormat = getattr(material, "dagormat", None)
        textures = getattr(dagormat, "textures", None)
        is_proxy = _is_proxy_dagormat(dagormat)
        if is_proxy:
            proxy_source, proxy = _proxy_source(material, dagormat)
            authored_rows = [
                (slot, _proxymat_texture_path(proxy_source, authored), None)
                for slot, authored in sorted(proxy.textures.items())
            ]
            if proxy.macro_textures:
                for asset_name in _material_macro_asset_names(material):
                    for slot, authored in sorted(proxy.macro_textures.items()):
                        expanded = authored.replace(
                            "$(ASSET_NAME)", asset_name)
                        if "$(" in expanded:
                            raise ProjectTextureError(
                                "MH_E_INVALID_RESOURCE_SOURCE",
                                f"material {material.name!r} / {slot}",
                                f"unsupported proxymat texture macro: "
                                f"{authored!r}")
                        authored_rows.append((
                            slot,
                            _proxymat_texture_path(proxy_source, expanded),
                            asset_name,
                        ))
            writable_textures = None
        elif textures is not None:
            authored_rows = [
                (f"tex{index}", getattr(textures, f"tex{index}", ""), None)
                for index in range(16)
            ]
            writable_textures = textures
        else:
            continue
        for slot, authored, macro_asset_name in authored_rows:
            if authored in (None, ""):
                continue
            if not isinstance(authored, str):
                raise ProjectTextureError(
                    "MH_E_INVALID_RESOURCE_SOURCE",
                    f"material {material.name!r} / dagormat.textures.{slot}",
                    f"texture path must be a string, got {authored!r}")
            source_path, transport_suffix = _split_transport_suffix(authored)
            try:
                absolute_source = _resolve_dagor_texture_source(
                    source_path, image_sources)
                plan = plan_project_texture(absolute_source, root)
            except ProjectTextureError as exc:
                raise ProjectTextureError(
                    exc.code,
                    f"material {material.name!r} / dagormat.textures.{slot} / "
                    f"{authored!r}",
                    exc.message,
                ) from exc
            bindings.append(DagorTextureBinding(
                material=material,
                material_name=material.name,
                slot=slot,
                authored_path=authored,
                transport_suffix=transport_suffix,
                plan=plan,
                writable_textures=writable_textures,
                macro_asset_name=macro_asset_name,
            ))
    return bindings


def copy_all_dagor_textures_to_project(
        *, source_root, materials=None) -> dict:
    """Copy all Dagor texture files while preserving the tree below assets."""
    material_rows = bpy.data.materials if materials is None else materials
    bindings = collect_dagor_texture_bindings(
        material_rows, source_root=source_root)
    report = atomic_copy_texture_plans(
        [binding.plan for binding in bindings], source_root=source_root)
    report.update({
        "materials": len({binding.material_name for binding in bindings}),
        "referenced_slots": len(bindings),
        "macro_slots": sum(
            binding.macro_asset_name is not None for binding in bindings),
        "unique_files": report["copied"] + report["skipped"],
    })
    return report


def _write_texture_slot(textures, slot, value):
    """Write one slot without firing dag4blend's per-slot RNA update callback.

    Every dag4blend ``texN`` StringProperty carries ``update=update_material``,
    which rebuilds the material's whole shader node tree on each attribute
    assignment - the reason a full-scene remap took seconds. Item assignment
    hits the same IDProperty storage the RNA getter reads while bypassing the
    callback; dag4blend's own ``update_tex_paths`` writes exactly this way.
    A carrier without item access falls back to plain attribute assignment.
    """
    try:
        textures[slot] = value
    except TypeError:
        setattr(textures, slot, value)


def remap_all_dagor_textures_to_project(
        *, source_root, materials=None) -> dict:
    """Point every Dagor texture slot at the copied project file or roll back."""
    material_rows = bpy.data.materials if materials is None else materials
    bindings = collect_dagor_texture_bindings(
        material_rows, source_root=source_root)
    validate_texture_plans(
        [binding.plan for binding in bindings],
        require_sources=False,
        require_destinations=True,
    )

    writable_bindings = [
        binding for binding in bindings
        if binding.writable_textures is not None
    ]
    snapshots = [
        (binding.writable_textures, binding.slot,
         getattr(binding.writable_textures, binding.slot))
        for binding in writable_bindings
    ]
    changed = 0
    try:
        for binding in writable_bindings:
            textures = binding.writable_textures
            remapped = str(binding.plan.destination) + binding.transport_suffix
            current = getattr(textures, binding.slot)
            if current == remapped:
                continue
            _write_texture_slot(textures, binding.slot, remapped)
            if getattr(textures, binding.slot) != remapped:
                raise RuntimeError(
                    f"texture path write did not persist: "
                    f"material {binding.material_name!r} / {binding.slot}")
            changed += 1
    except Exception as apply_error:
        rollback_errors = []
        for textures, slot, previous in reversed(snapshots):
            try:
                _write_texture_slot(textures, slot, previous)
                if getattr(textures, slot) != previous:
                    raise RuntimeError("rollback read-back differs")
            except Exception as rollback_error:
                rollback_errors.append(f"{slot}: {rollback_error}")
        if rollback_errors:
            raise RuntimeError(
                f"texture path remap failed: {apply_error}; rollback also "
                "failed: " + "; ".join(rollback_errors)
            ) from apply_error
        raise

    return {
        "ok": True,
        "materials": len({binding.material_name for binding in bindings}),
        "referenced_slots": len(bindings),
        "macro_slots": sum(
            binding.macro_asset_name is not None for binding in bindings),
        "read_only_proxy_slots": len(bindings) - len(writable_bindings),
        "remapped": changed,
        "paths": sorted({
            str(binding.plan.destination) + binding.transport_suffix
            for binding in bindings
        }),
    }
