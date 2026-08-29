"""Blender adapter for copying and remapping all authored Dagor textures."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path

import bpy

from ..core.project_textures import (
    ProjectTextureError,
    TextureCopyPlan,
    atomic_copy_texture_plans,
    plan_project_texture,
    validate_texture_plans,
)

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


def collect_dagor_texture_bindings(
        materials, *, source_root) -> list[DagorTextureBinding]:
    """Preflight every non-empty tex0..tex15 path in deterministic order."""
    root = _project_root(source_root)
    bindings = []
    for material in sorted(materials, key=lambda value: value.name):
        dagormat = getattr(material, "dagormat", None)
        textures = getattr(dagormat, "textures", None)
        if textures is None:
            continue
        for index in range(16):
            slot = f"tex{index}"
            authored = getattr(textures, slot, "")
            if authored in (None, ""):
                continue
            if not isinstance(authored, str):
                raise ProjectTextureError(
                    "MH_E_INVALID_RESOURCE_SOURCE",
                    f"material {material.name!r} / dagormat.textures.{slot}",
                    f"texture path must be a string, got {authored!r}")
            source_path, transport_suffix = _split_transport_suffix(authored)
            absolute_source = bpy.path.abspath(source_path)
            try:
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

    snapshots = [
        (binding.material.dagormat.textures, binding.slot,
         getattr(binding.material.dagormat.textures, binding.slot))
        for binding in bindings
    ]
    changed = 0
    try:
        for binding in bindings:
            textures = binding.material.dagormat.textures
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
        "remapped": changed,
        "paths": sorted({
            str(binding.plan.destination) + binding.transport_suffix
            for binding in bindings
        }),
    }
