"""Blender adapter and crash-safe writer for Source Protocol v4 materials."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path

import bpy

from ..core.canonical import validate_resource_name
from ..core.canonical_json import narrow_float32
from ..core.materials import (
    MATERIAL_TEXTURE_EXTENSIONS,
    MaterialValueError,
    material_json_bytes,
    parse_material,
    resolve_texture_reference,
)
from ..core.mesh_nodes import strip_blender_duplicate_suffix
from ..core.model import MaterialResource
from ..core.payload_publish_v2 import atomic_publish_bytes
from ..core.validate import MHValidationError
from .readonly_properties import existing_property_group

__all__ = [
    "MaterialBinding",
    "PreparedMaterialExport",
    "TECHNICAL_MATERIAL_NAMES",
    "TECHNICAL_MATERIAL_SHADER_CLASSES",
    "apply_material_resource",
    "is_technical_material",
    "material_class_for_export",
    "material_content_fingerprint",
    "prepare_blender_material_export",
    "read_material_file",
    "resolve_material_binding",
    "write_prepared_material",
]


# Owner decision 2026-08-30 (docs/15 §1.3): `cls` is a Blender-only technical
# material (solid green, Dagor shader `gi_black`, never rendered). It reaches
# neither an FBX material slot nor the `.material` closure.
TECHNICAL_MATERIAL_NAMES = frozenset({"cls"})
TECHNICAL_MATERIAL_SHADER_CLASSES = frozenset({"gi_black"})


@dataclass(frozen=True)
class PreparedMaterialExport:
    resource: MaterialResource
    target: Path
    payload: bytes


@dataclass(frozen=True)
class MaterialBinding:
    """One transported material: its logical name and its authoring datablock.

    ``name`` is the canonical logical name (``docs/15 §2.3``): the Blender
    ``.NNN`` duplicate suffix is a datablock-namespace artifact and never
    reaches the transport.  ``material`` is the single representative datablock
    that publishes the payload for that name.
    """

    name: str
    material: object
    # Owner decision 2026-08-30: diverging ``.NNN`` claimants merge into the
    # base logical name; this carries the one warning row describing what the
    # representative overrode, or ``None`` when the group agrees.
    divergence: tuple = None

    @property
    def library(self):
        return self.material.library


# Publication planners compare bindings by identity to detect two different
# Blender materials claiming one token, so one representative datablock must
# always yield one binding object across every mesh in a closure.
_BINDINGS: dict[str, MaterialBinding] = {}


def _material_datablock(material):
    """Accept either a Blender material or an already resolved binding."""
    return material.material if isinstance(material, MaterialBinding) else material


def _logical_material_name(material) -> str:
    if isinstance(material, MaterialBinding):
        return material.name
    return strip_blender_duplicate_suffix(str(material.name))


def is_technical_material(material) -> bool:
    """Return whether this material is Dagor technical geometry paint."""
    if material is None:
        return False
    datablock = _material_datablock(material)
    if _logical_material_name(datablock) in TECHNICAL_MATERIAL_NAMES:
        return True
    dagormat = _authored_dagormat(datablock)
    if dagormat is None:
        return False
    return str(getattr(dagormat, "shader_class", "") or "") in (
        TECHNICAL_MATERIAL_SHADER_CLASSES)


def _fingerprint_value(value):
    if isinstance(value, list):
        return tuple(narrow_float32(component) for component in value)
    return narrow_float32(value)


def material_content_fingerprint(material) -> tuple:
    """Return the identity-free v4 content of one material.

    Numbers are narrowed to the float32 the canonical payload stores, so two
    authored spellings of the same value (``7.1`` and ``7.0999999``) compare
    equal, while any real divergence stays visible.
    """
    resource = _extract_resource(_material_datablock(material))
    return (
        resource.library,
        resource.material_class,
        resource.twosided,
        tuple(sorted(resource.textures.items())),
        tuple(sorted(
            (name, _fingerprint_value(value))
            for name, value in resource.params.items())),
    )


_FINGERPRINT_FIELDS = ("library", "class", "twosided", "textures", "params")


def _fingerprint_difference(left: tuple, right: tuple) -> str:
    """Name what actually diverges so the artist can fix the right field."""
    differences = []
    for field, first, second in zip(_FINGERPRINT_FIELDS, left, right):
        if first == second:
            continue
        if field in ("textures", "params"):
            keys = sorted(
                set(dict(first)) | set(dict(second))
                if isinstance(first, tuple) else ())
            changed = [
                f"{field}.{key} {dict(first).get(key)!r} vs "
                f"{dict(second).get(key)!r}"
                for key in keys
                if dict(first).get(key) != dict(second).get(key)]
            differences.extend(changed)
        else:
            differences.append(f"{field} {first!r} vs {second!r}")
    return "; ".join(differences) or "content differs"


def _duplicate_group(logical_name: str) -> list:
    """Every scene material claiming one logical name, in a stable order.

    The group is deliberately file-wide rather than limited to the meshes being
    exported. ``<name>.material`` is one file in the Source Root, so a second
    datablock claiming that name is a conflict no matter which mesh transports
    it, and a file-wide group is what makes one representative — and therefore
    one binding identity — well defined for every mesh in a closure.
    """
    return sorted(
        (material for material in bpy.data.materials
         if strip_blender_duplicate_suffix(material.name) == logical_name),
        key=lambda material: material.name)


def resolve_material_binding(material) -> MaterialBinding:
    """Merge Blender ``.NNN`` duplicates onto one representative datablock.

    ``X.001`` is a Blender duplicate of ``X``, not a second resource: the pair
    publishes one ``X.material`` and the FBX slot is written as ``X``. Owner
    decision 2026-08-30: this holds even when the duplicate's v4 content
    diverges - the representative (the base-named datablock when it exists) is
    the published authority for every object, and the divergence is reported
    as a warning naming the overridden fields, never refused. The dropped
    parameters stay authored in the scene datablocks.
    """
    if isinstance(material, MaterialBinding):
        return material
    logical_name = _logical_material_name(material)
    group = _duplicate_group(logical_name)
    if not group:  # pragma: no cover - a live datablock is always its own group
        group = [material]
    exact = [row for row in group if row.name == logical_name]
    representative = exact[0] if exact else group[0]
    divergence = None
    if len(group) > 1:
        reference = material_content_fingerprint(representative)
        diverging = []
        for other in group:
            if other == representative:
                continue
            candidate = material_content_fingerprint(other)
            if candidate != reference:
                diverging.append(
                    f"'{other.name}' "
                    f"({_fingerprint_difference(candidate, reference)})")
        if diverging:
            divergence = (
                "MH_W_DAGOR_CONSTRUCT_DROPPED",
                (logical_name,
                 *[row.name for row in group if row != representative]),
                f"materials merged into '{logical_name}': "
                f"'{representative.name}' is the published authority; "
                "overridden: " + "; ".join(diverging))
    cached = _BINDINGS.get(logical_name)
    if cached is not None:
        try:
            if cached.material == representative and cached.divergence == divergence:
                return cached
        except ReferenceError:
            pass
    binding = MaterialBinding(
        name=logical_name, material=representative, divergence=divergence)
    _BINDINGS[logical_name] = binding
    return binding


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
    source = bpy.path.abspath(authored_path) if authored_path else image.name
    token = _texture_token_from_path(source, path)
    return token


def _texture_token_from_path(authored_path, path: str) -> str:
    if not isinstance(authored_path, str) or not authored_path:
        raise MaterialValueError(
            "MH_E_NONCANONICAL_TEXTURE_REFERENCE", path,
            "texture path must identify an extensionless logical name")
    filename = authored_path.replace("\\", "/").rsplit("/", 1)[-1]
    # Dagor permits transport suffixes such as ``*?q0-0-1``.  They are not
    # part of resource identity; the filename stem remains the v4 token.
    filename = filename.split("*?", 1)[0]
    source_name = Path(filename)
    if (source_name.suffix
            and source_name.suffix.lower() not in MATERIAL_TEXTURE_EXTENSIONS):
        raise MaterialValueError(
            "MH_E_NONCANONICAL_TEXTURE_REFERENCE", path,
            f"texture path uses unsupported extension {source_name.suffix!r}")
    token = source_name.stem if source_name.suffix else source_name.name
    # The codec performs the exact fail-closed token validation.
    try:
        parse_material({"class": "probe", "textures": {"tex0": token}})
    except MaterialValueError as exc:
        raise MaterialValueError(exc.code, path, exc.message) from exc
    return token


def _authored_dagormat(material):
    dagormat = existing_property_group(material, "dagormat")
    shader_class = getattr(dagormat, "shader_class", "")
    if shader_class in (None, "", "None"):
        return None
    return dagormat


def _dagor_texture_paths(dagormat) -> dict[str, str]:
    textures = existing_property_group(dagormat, "textures")
    if textures is None:
        return {}
    out = {}
    for index in range(16):
        slot = f"tex{index}"
        authored_path = getattr(textures, slot, "")
        if authored_path in (None, ""):
            continue
        out[slot] = authored_path
    return out


def _not_roundtrippable(path: str, message: str) -> MaterialValueError:
    return MaterialValueError(
        "MH_E_MATERIAL_NOT_ROUNDTRIPPABLE", path, message)


def _dagor_parameter_value(value, path: str):
    to_list = getattr(value, "to_list", None)
    if callable(to_list):
        value = list(to_list())
    elif isinstance(value, tuple):
        value = list(value)

    if isinstance(value, bool):
        raise _not_roundtrippable(
            path, "Dagor bool parameters are not Source Protocol v4 scalars")
    if isinstance(value, (int, float)):
        return value
    if not isinstance(value, list):
        raise _not_roundtrippable(
            path, f"Dagor parameter type {type(value).__name__} is unsupported")
    if len(value) != 4:
        raise _not_roundtrippable(
            path, "Dagor vector parameter must contain exactly four numbers")
    if any(isinstance(component, bool)
           or not isinstance(component, (int, float)) for component in value):
        raise _not_roundtrippable(
            path, "Dagor vector parameter contains a non-numeric component")
    return value


def _dagor_params(dagormat) -> dict:
    optional = existing_property_group(dagormat, "optional")
    if optional is None:
        return {}
    try:
        names = list(optional.keys())
    except (AttributeError, TypeError) as exc:
        raise MaterialValueError(
            "MH_E_MATERIAL_GRAMMAR", "dagormat.optional",
            "optional parameters must be a named property mapping") from exc
    return {name: optional[name] for name in names}


def _dagor_twosided(dagormat) -> bool:
    sides = getattr(dagormat, "sides", None)
    if type(sides) is not int:
        raise _not_roundtrippable(
            "dagormat.sides",
            f"Dagor sides must be an integer, got {sides!r}")
    if sides == 0:
        return False
    if sides == 1:
        return True
    if sides == 2:
        raise _not_roundtrippable(
            "dagormat.sides",
            "real_two_sided cannot be represented by Source Protocol v4")
    raise _not_roundtrippable(
        "dagormat.sides",
        f"unsupported Dagor sides value {sides!r}")


def material_class_for_export(material) -> str:
    """Return the authored class token without guessing or repairing it.

    The dedicated v4 override remains authoritative.  When it is empty,
    reuse dag4blend's semantic shader class so artists do not have to enter
    the same token twice.  Dagor's ``None`` sentinel means "not authored";
    every other value is returned verbatim and is still checked by the
    strict v4 codec.
    """
    settings = existing_property_group(material, "mh4blend")
    explicit = getattr(settings, "material_class", "")
    if explicit != "":
        return explicit

    dagormat = _authored_dagormat(material)
    return "" if dagormat is None else dagormat.shader_class


def _extract_resource(material) -> MaterialResource:
    material = _material_datablock(material)
    # Preserve the canonical name diagnostic verbatim; identity is external to
    # the material grammar and must not be reclassified as a codec failure.
    logical_name = _logical_material_name(material)
    validate_resource_name(logical_name)
    if not hasattr(type(material) if isinstance(material, bpy.types.ID) else material,
                   "mh4blend"):
        raise MaterialValueError(
            "MH_E_MATERIAL_GRAMMAR", material.name,
            "material has no registered mh4blend property group")
    settings = existing_property_group(material, "mh4blend")
    mode = getattr(settings, "mode", "CLASS")
    if mode == "LIBRARY":
        if settings.twosided_override or settings.textures or settings.params:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", material.name,
                "library material cannot contain local overrides")
        return MaterialResource(name=logical_name, library=settings.library)
    if mode != "CLASS":
        raise MaterialValueError(
            "MH_E_MATERIAL_GRAMMAR", "mode", "must be CLASS or LIBRARY")

    dagormat = _authored_dagormat(material)
    dagor_texture_paths = (
        _dagor_texture_paths(dagormat) if dagormat is not None else {})
    textures = {}
    explicit_texture_slots = set()
    for row in getattr(settings, "textures", ()):
        slot = f"tex{row.slot}"
        if slot in explicit_texture_slots:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", f"textures.{slot}",
                "duplicate texture slot")
        explicit_texture_slots.add(slot)
        textures[slot] = _texture_token(row.image, f"textures.{slot}")
    for slot, authored_path in dagor_texture_paths.items():
        if slot in explicit_texture_slots:
            continue
        textures[slot] = _texture_token_from_path(
            authored_path, f"dagormat.textures.{slot}")

    params = _dagor_params(dagormat) if dagormat is not None else {}
    explicit_param_names = set()
    for row in getattr(settings, "params", ()):
        if row.name in explicit_param_names:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", f"params.{row.name}",
                "duplicate parameter")
        explicit_param_names.add(row.name)
        params[row.name] = (
            row.scalar if row.kind == "SCALAR" else list(row.vector))
    for name in params.keys() - explicit_param_names:
        params[name] = _dagor_parameter_value(
            params[name], f"dagormat.optional.{name}")

    twosided = None
    if getattr(settings, "twosided_override", False):
        twosided = settings.twosided
    elif dagormat is not None:
        twosided = _dagor_twosided(dagormat)

    return MaterialResource(
        name=logical_name,
        material_class=material_class_for_export(material),
        twosided=twosided,
        textures=textures,
        params=params,
    )


def prepare_blender_material_export(
        material, output_dir, *, source_root) -> PreparedMaterialExport:
    """Extract and fully validate one Blender material without writing files."""
    if material is None:
        raise ValueError("material is required")
    material = _material_datablock(material)
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
