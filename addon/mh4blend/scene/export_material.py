"""Blender adapter and crash-safe writer for Source Protocol v4 materials."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re

import bpy

from ..core.canonical import validate_resource_name
from ..core.canonical_json import narrow_float32
from ..core.dagor_names import (
    project_dagor_material_name,
    project_dagor_resource_name,
)
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
from ..core.proxymat import read_proxymat
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
_DAGOR_PARAMETER_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$", re.ASCII)
_BLENDER_ID_NAME_MAX_BYTES = 63
_TRANSPORT_NAME_HASH_CHARS = 12


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
    divergence: tuple = None
    # Dagor proxymats may use ``$(ASSET_NAME)`` texture templates.  The macro
    # is expanded only at a concrete mesh-resource boundary; a standalone
    # material has no authority to guess this value.
    macro_asset_name: str | None = None

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


def _authored_material_name(material) -> str:
    return strip_blender_duplicate_suffix(str(material.name))


def _uses_dagor_name_boundary(material) -> bool:
    dagormat = existing_property_group(material, "dagormat")
    if dagormat is None:
        return False
    if getattr(dagormat, "is_proxy", False) is True:
        return True
    shader_class = str(getattr(dagormat, "shader_class", "") or "")
    return shader_class not in ("", "None")


def _logical_material_name(material) -> str:
    if isinstance(material, MaterialBinding):
        return material.name
    authored_name = _authored_material_name(material)
    if _uses_dagor_name_boundary(material):
        return project_dagor_material_name(authored_name)
    return authored_name


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
    if isinstance(value, (str, bool)):
        return value
    if isinstance(value, list):
        return tuple(narrow_float32(component) for component in value)
    return narrow_float32(value)


def material_content_fingerprint(material) -> tuple:
    """Return the identity-free v4 content of one material.

    Numbers are narrowed to the float32 the canonical payload stores, so two
    authored spellings of the same value (``7.1`` and ``7.0999999``) compare
    equal, while any real divergence stays visible.
    """
    datablock = _material_datablock(material)
    # Macro proxymats are templates.  A fixed synthetic asset token makes
    # their identity-free content comparable without choosing a real owner.
    proxy = _proxy_dagormat(datablock)
    comparable = (
        MaterialBinding(
            name=_logical_material_name(datablock),
            material=datablock,
            macro_asset_name="asset",
        )
        if proxy is not None else datablock
    )
    resource = _extract_resource(comparable)
    return (
        resource.library,
        resource.material_class,
        resource.twosided,
        tuple(sorted(resource.textures.items())),
        tuple(sorted(
            (name, _fingerprint_value(value))
            for name, value in resource.params.items())),
    )


def _projected_claim_group(logical_name: str) -> list:
    """Return every scene material whose external name projects to one token."""
    claimants = []
    for candidate in bpy.data.materials:
        try:
            candidate_name = _logical_material_name(candidate)
        except (ReferenceError, TypeError, ValueError):
            continue
        if candidate_name == logical_name:
            claimants.append(candidate)
    return sorted(claimants, key=lambda candidate: candidate.name)


def _disambiguated_material_name(logical_name: str, representative) -> str:
    digest = hashlib.sha256(
        representative.name.encode("utf-8")).hexdigest()[:12]
    projected = _fit_blender_transport_name(f"{logical_name}_{digest}")
    validate_resource_name(projected)
    return projected


def _fit_blender_transport_name(name: str) -> str:
    """Keep a canonical binding inside Blender's 63-byte ID-name limit."""
    validate_resource_name(name)
    encoded = name.encode("ascii")
    if len(encoded) <= _BLENDER_ID_NAME_MAX_BYTES:
        return name
    digest = hashlib.sha256(encoded).hexdigest()[:_TRANSPORT_NAME_HASH_CHARS]
    prefix_bytes = (
        _BLENDER_ID_NAME_MAX_BYTES - 1 - _TRANSPORT_NAME_HASH_CHARS)
    shortened = f"{name[:prefix_bytes]}_{digest}"
    validate_resource_name(shortened)
    return shortened


def _binding_for_macro_asset(
        binding: MaterialBinding, asset_name: str | None) -> MaterialBinding:
    if asset_name is None:
        return binding
    validate_resource_name(asset_name)
    dagormat = _proxy_dagormat(binding.material)
    if dagormat is None:
        return binding
    _source, proxy = _read_proxy_source(
        binding.material, _authored_material_name(binding.material), dagormat)
    if not proxy.macro_textures:
        return binding
    name = _fit_blender_transport_name(
        f"{binding.name}__{asset_name}")
    validate_resource_name(name)
    cached = _BINDINGS.get(name)
    if cached is not None:
        try:
            if (cached.material == binding.material
                    and cached.macro_asset_name == asset_name):
                return cached
        except ReferenceError:
            pass
    specialized = MaterialBinding(
        name=name,
        material=binding.material,
        divergence=binding.divergence,
        macro_asset_name=asset_name,
    )
    _BINDINGS[name] = specialized
    return specialized


def resolve_material_binding(
        material, *, asset_name: str | None = None) -> MaterialBinding:
    """Bind equivalent aliases together and preserve divergent claimants.

    All scene materials projecting to one base token are compared by canonical
    v4 content. Equivalent ``.NNN``/case/punctuation aliases share one binding.
    Distinct content receives a deterministic suffix derived from the stable
    Blender datablock name, so no material semantics are discarded.
    """
    if isinstance(material, MaterialBinding):
        return _binding_for_macro_asset(material, asset_name)
    logical_name = _logical_material_name(material)
    if not isinstance(material, bpy.types.ID):
        return _binding_for_macro_asset(
            MaterialBinding(name=logical_name, material=material), asset_name)
    claimants = _projected_claim_group(logical_name)
    if not claimants:  # pragma: no cover - a live datablock claims itself
        claimants = [material]
    if len(claimants) == 1:
        representative = claimants[0]
        cached = _BINDINGS.get(logical_name)
        if cached is not None:
            try:
                if cached.material == representative and cached.divergence is None:
                    return _binding_for_macro_asset(cached, asset_name)
            except ReferenceError:
                pass
        binding = MaterialBinding(
            name=logical_name, material=representative, divergence=None)
        _BINDINGS[logical_name] = binding
        return _binding_for_macro_asset(binding, asset_name)

    fingerprints = {
        candidate.as_pointer(): material_content_fingerprint(candidate)
        for candidate in claimants
    }
    exact = [candidate for candidate in claimants
             if candidate.name == logical_name]
    authority = exact[0] if exact else claimants[0]
    primary_fingerprint = fingerprints[authority.as_pointer()]
    material_fingerprint = fingerprints[material.as_pointer()]
    equivalent = [
        candidate for candidate in claimants
        if fingerprints[candidate.as_pointer()] == material_fingerprint]
    representative = (
        authority if material_fingerprint == primary_fingerprint
        else equivalent[0])
    binding_name = (
        logical_name if material_fingerprint == primary_fingerprint
        else _disambiguated_material_name(logical_name, representative))

    cached = _BINDINGS.get(binding_name)
    if cached is not None:
        try:
            if cached.material == representative and cached.divergence is None:
                return _binding_for_macro_asset(cached, asset_name)
        except ReferenceError:
            pass
    binding = MaterialBinding(
        name=binding_name, material=representative, divergence=None)
    _BINDINGS[binding_name] = binding
    return _binding_for_macro_asset(binding, asset_name)


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


def _texture_token_from_path(
        authored_path, path: str, *, project_dagor_case: bool = False) -> str:
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
    if project_dagor_case:
        try:
            token = project_dagor_resource_name(token)
        except (TypeError, ValueError) as exc:
            raise MaterialValueError(
                "MH_E_NONCANONICAL_TEXTURE_REFERENCE", path,
                "Dagor texture filename stem must contain only ASCII "
                "letters, digits, underscore and projectable whitespace") from exc
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


def _proxy_dagormat(material):
    """Return dagormat when the datablock denotes a proxymat source."""
    dagormat = existing_property_group(material, "dagormat")
    if dagormat is None:
        return None
    shader_class = str(getattr(dagormat, "shader_class", "") or "")
    if getattr(dagormat, "is_proxy", False) is True:
        return dagormat
    return dagormat if shader_class.endswith(":proxymat") else None


def _read_proxy_source(material, authored_name: str, dagormat):
    directory = str(getattr(dagormat, "proxy_path", "") or "")
    source = Path(bpy.path.abspath(directory)).resolve(strict=False)
    source = source / f"{authored_name}.proxymat.blk"
    try:
        proxy = read_proxymat(source)
    except (OSError, UnicodeError) as exc:
        raise MaterialValueError(
            "MH_E_INVALID_RESOURCE_SOURCE", str(source),
            "proxymat source cannot be read; check proxy_path or run the "
            "dag4blend proxymat search") from exc
    return source, proxy


def _proxy_resource(
        material, logical_name: str, authored_name: str,
        dagormat, *, macro_asset_name: str | None = None) -> MaterialResource:
    source, proxy = _read_proxy_source(material, authored_name, dagormat)

    if proxy.macro_textures:
        if macro_asset_name is None:
            details = ", ".join(
                f"{slot}={value!r}"
                for slot, value in sorted(proxy.macro_textures.items()))
            raise _not_roundtrippable(
                str(source),
                "proxymat macro textures require a concrete mesh asset "
                f"context before publication ({details})")
        validate_resource_name(macro_asset_name)

    authored_textures = dict(proxy.textures)
    for slot, value in proxy.macro_textures.items():
        expanded = value.replace("$(ASSET_NAME)", macro_asset_name)
        if "$(" in expanded:
            raise _not_roundtrippable(
                str(source),
                f"proxymat texture {slot} contains an unsupported macro: "
                f"{value!r}")
        authored_textures[slot] = expanded
    textures = {
        slot: _texture_token_from_path(
            value, f"proxymat.{slot}", project_dagor_case=True)
        for slot, value in authored_textures.items()
    }
    projected_params = _project_dagor_parameter_names(
        proxy.params, "proxymat.script")
    params = {
        name: _dagor_parameter_value(value, f"proxymat.script.{name}")
        for name, value in projected_params.items()
    }
    return MaterialResource(
        name=logical_name,
        material_class=proxy.material_class,
        twosided=proxy.twosided,
        textures=textures,
        params=params,
    )


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
        return value
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
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


def _project_dagor_parameter_names(values, path: str) -> dict:
    """Project external Dagor parameter case at the adapter boundary.

    Source Protocol remains strict lowercase.  Dagor identifiers may contain
    ASCII uppercase letters, which are lowered without changing separators or
    inventing word boundaries.  Distinct authored keys that converge after
    lowering are ambiguous and therefore fail before publication.
    """
    names = list(values.keys())
    for name in names:
        if (not isinstance(name, str)
                or _DAGOR_PARAMETER_NAME_RE.fullmatch(name) is None):
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", f"{path}.{name}",
                "Dagor parameter name must contain only ASCII letters, "
                "digits and underscore")
    projected = {}
    authored = {}
    for name in sorted(names):
        canonical = name.lower()
        previous = authored.get(canonical)
        if previous is not None and previous != name:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", path,
                f"Dagor parameters {previous!r} and {name!r} both project "
                f"to canonical key {canonical!r}")
        authored[canonical] = name
        projected[canonical] = values[name]
    return projected


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
    return _project_dagor_parameter_names(
        {name: optional[name] for name in names}, "dagormat.optional")


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
        # Dagor implements real_two_sided with duplicated, flipped geometry.
        # Source Protocol v5 has one two-sided material flag, whose UE importer
        # applies the material-instance TwoSided base-property override.  That
        # is the approved projection for foliage and other thin surfaces.
        return True
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
    # Preserve the canonical name diagnostic verbatim; identity is external to
    # the material grammar and must not be reclassified as a codec failure.
    logical_name = _logical_material_name(material)
    macro_asset_name = (
        material.macro_asset_name
        if isinstance(material, MaterialBinding) else None)
    material = _material_datablock(material)
    validate_resource_name(logical_name)
    proxy_dagormat = _proxy_dagormat(material)
    if proxy_dagormat is not None:
        return _proxy_resource(
            material, logical_name, _authored_material_name(material),
            proxy_dagormat, macro_asset_name=macro_asset_name)
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
            authored_path, f"dagormat.textures.{slot}",
            project_dagor_case=True)

    params = _dagor_params(dagormat) if dagormat is not None else {}
    explicit_param_names = set()
    for row in getattr(settings, "params", ()):
        if row.name in explicit_param_names:
            raise MaterialValueError(
                "MH_E_MATERIAL_GRAMMAR", f"params.{row.name}",
                "duplicate parameter")
        explicit_param_names.add(row.name)
        params[row.name] = (
            row.scalar if row.kind == "SCALAR"
            else row.string if row.kind == "STRING"
            else row.boolean if row.kind == "BOOLEAN"
            else list(row.vector))
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
    binding = resolve_material_binding(material)
    material = _material_datablock(binding)
    root = _resolved_root(source_root)
    output = Path(bpy.path.abspath(os.fspath(output_dir))).resolve(strict=False)
    if not _inside(root, output):
        raise ValueError("Material output folder must be inside Project Source Root")

    try:
        resource = _extract_resource(binding)
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
        if isinstance(value, bool):
            row.kind = "BOOLEAN"
            row.boolean = value
        elif isinstance(value, str):
            row.kind = "STRING"
            row.string = value
        elif isinstance(value, list):
            row.kind = "VECTOR"
            row.vector = value
        else:
            row.kind = "SCALAR"
            row.scalar = value
