"""Pure Source Protocol v5 composite codec and source-closure gates."""

from __future__ import annotations

import math
from pathlib import Path
import re
from typing import Any, Callable, Iterable, Mapping

from .canonical_json import (
    CanonicalJSONDuplicateKey,
    CanonicalJSONNonFinite,
    CanonicalJSONSyntaxError,
    canonical_json_bytes,
    narrow_float32,
    parse_json,
)
from .model import (
    Composite,
    CompositeTransform,
    IDENTITY_TRANSFORM,
    Node,
    RandomOption,
)

__all__ = [
    "CompositeValueError",
    "composite_document",
    "composite_json_bytes",
    "iter_profile_references",
    "iter_resource_references",
    "parse_composite",
    "read_composite_file",
    "validate_composite_cycles",
]


_TOKEN_RE = re.compile(r"^[a-z0-9_]+$")
_KINDS = frozenset({"mesh", "actor", "composite", "group", "random", "gameobj"})
_RESOURCE_KINDS = frozenset({"mesh", "actor", "composite", "gameobj"})
_OPTION_KINDS = frozenset({"mesh", "actor", "composite", "empty", "gameobj"})
_NODE_FIELDS = frozenset({
    "kind", "resource", "name", "transform", "profile", "placement",
    "place_type", "appearance_seed_boundary", "options", "children",
})
_OPTION_FIELDS = frozenset({"kind", "resource", "weight"})
_TRANSFORM_FIELDS = frozenset({"translation_cm", "rotation_quat", "scale"})
_IDENTITY_TRANSLATION = (0.0, 0.0, 0.0)
_IDENTITY_ROTATION = (0.0, 0.0, 0.0, 1.0)
_IDENTITY_SCALE = (1.0, 1.0, 1.0)
_QUATERNION_NORM_TOLERANCE = 1.0e-3
# One float64 normalization narrowed to four float32 components can leave the
# represented norm a few e-8 away from one.  Treat that output band as the
# canonical fixed point so canonical read-back never normalizes it by another
# ULP.  Values outside the band still take the existing normalization path.
_QUATERNION_CANONICAL_NORM_TOLERANCE = 1.0e-7
_LEGACY_MESSAGE = "файл прежнего поколения: удалите и переэкспортируйте"


class CompositeValueError(ValueError):
    """A composite cannot be represented without losing v5 semantics."""

    def __init__(self, code: str, path: str, message: str):
        self.code = code
        self.path = path
        super().__init__(f"{code}: {path}: {message}")


def _coded_error(code: str, path: str, message: str) -> CompositeValueError:
    return CompositeValueError(code, path, message)


def _error(path: str, message: str) -> CompositeValueError:
    return _coded_error("MH_E_COMPOSITE_GRAMMAR", path, message)


def _token(value: Any, path: str) -> str:
    if not isinstance(value, str) or _TOKEN_RE.fullmatch(value) is None:
        raise _error(path, "must match [a-z0-9_]+ exactly")
    return value


def _number(value: Any, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise _error(path, "must be a number")
    try:
        finite = math.isfinite(value)
    except (OverflowError, TypeError, ValueError) as exc:
        raise _error(path, str(exc)) from exc
    if not finite:
        raise _coded_error(
            "MH_E_NAN_INF_VALUE", path, "NaN/Inf is not a composite value")
    try:
        return narrow_float32(value)
    except (TypeError, ValueError) as exc:
        raise _error(path, "must be representable as finite IEEE float32") from exc


def _vector(value: Any, length: int, path: str) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != length:
        raise _error(path, f"must contain exactly {length} numbers")
    return tuple(
        _number(component, f"{path}[{index}]")
        for index, component in enumerate(value)
    )


def _canonical_quaternion(
    value: Any,
    path: str,
    *,
    require_unit: bool,
) -> tuple[float, ...]:
    components = _vector(value, 4, path)
    norm = math.sqrt(sum(component * component for component in components))
    if norm == 0.0 or (
        require_unit and abs(norm - 1.0) > _QUATERNION_NORM_TOLERANCE
    ):
        raise _error(path, "quaternion norm must be within 1e-3 of one")
    normalized = (
        components
        if abs(norm - 1.0) <= _QUATERNION_CANONICAL_NORM_TOLERANCE
        else tuple(narrow_float32(component / norm) for component in components)
    )
    narrowed_norm = math.sqrt(sum(component * component for component in normalized))
    if narrowed_norm == 0.0:
        raise _error(path, "quaternion must be nonzero")
    negate = normalized[3] < 0.0
    if normalized[3] == 0.0:
        first_nonzero = next(
            (component for component in normalized[:3] if component != 0.0), 0.0)
        negate = first_nonzero < 0.0
    if negate:
        normalized = tuple(narrow_float32(-component) for component in normalized)
    return normalized


def _transform(value: Any, path: str, *, reader: bool) -> CompositeTransform:
    if not isinstance(value, dict):
        raise _error(path, "must be an object")
    unknown = set(value) - _TRANSFORM_FIELDS
    if unknown:
        raise _error(path, f"unknown field(s): {', '.join(sorted(unknown))}")
    translation = (
        _vector(value["translation_cm"], 3, f"{path}.translation_cm")
        if "translation_cm" in value else _IDENTITY_TRANSLATION)
    rotation = (
        _canonical_quaternion(
            value["rotation_quat"], f"{path}.rotation_quat", require_unit=reader)
        if "rotation_quat" in value else _IDENTITY_ROTATION)
    scale = (
        _vector(value["scale"], 3, f"{path}.scale")
        if "scale" in value else _IDENTITY_SCALE)
    if any(component == 0.0 for component in scale):
        raise _coded_error(
            "MH_E_INVALID_SCALE", f"{path}.scale", "scale components must be non-zero")
    return CompositeTransform(translation, rotation, scale)


def _place_type(value: Any, path: str) -> int:
    """Admit one non-negative integer place_type as opaque source provenance.

    Values outside the known ``0…6`` Dagor range are provenance and pass
    unblocked; the field is never executed. Absence is decided by the caller
    and is not the same statement as the explicit value zero.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        raise _error(path, "place_type must be an integer")
    if value < 0:
        raise _error(path, "place_type must be greater than or equal to zero")
    if value > 2147483647:
        # The UE carrier stores this value in an int32; a wider integer could
        # not round-trip and the two codecs must accept the same language.
        raise _error(path, "place_type must be representable as a 32-bit integer")
    return value


def _appearance_seed_boundary(value: Any, path: str) -> bool:
    if not isinstance(value, bool):
        raise _error(path, "appearance_seed_boundary must be a boolean")
    return value


def _rebased_placement_path(node_path: str, placement_path: str) -> str:
    if placement_path.startswith("$"):
        return node_path + placement_path[1:]
    return f"{node_path}.{placement_path}"


def _inline_placement_from_document(value: Any, path: str) -> "PlacementProfile":
    from .placements import PlacementValueError, parse_placement_profile
    if not isinstance(value, dict):
        raise _error(path, "inline placement must be an object")
    try:
        return parse_placement_profile(value, name="")
    except PlacementValueError as exc:
        raise CompositeValueError(
            exc.code,
            _rebased_placement_path(path, exc.path),
            str(exc).split(": ", 2)[-1],
        ) from exc


def _inline_placement_document(value: Any, path: str) -> dict[str, Any]:
    from .placements import PlacementValueError, placement_document
    try:
        return placement_document(value)
    except (PlacementValueError, TypeError) as exc:
        placement_path = getattr(exc, "path", "$")
        raise CompositeValueError(
            getattr(exc, "code", "MH_E_COMPOSITE_GRAMMAR"),
            _rebased_placement_path(path, placement_path),
            str(exc).split(": ", 2)[-1],
        ) from exc


def _option_from_document(value: Any, path: str) -> RandomOption:
    if not isinstance(value, dict):
        raise _error(path, "option must be an object")
    unknown = set(value) - _OPTION_FIELDS
    if unknown:
        raise _error(path, f"unknown field(s): {', '.join(sorted(unknown))}")
    if "kind" not in value or "weight" not in value:
        raise _error(path, "option requires fields 'kind' and 'weight'")
    kind = value["kind"]
    if not isinstance(kind, str) or kind not in _OPTION_KINDS:
        raise _coded_error(
            "MH_E_UNSUPPORTED_NODE_KIND", f"{path}.kind",
            f"unsupported random option kind {kind!r}")
    if kind == "empty":
        if "resource" in value:
            raise _error(path, "empty option forbids field 'resource'")
        resource = None
    else:
        if "resource" not in value:
            raise _error(path, f"{kind} option requires field 'resource'")
        resource = _token(value["resource"], f"{path}.resource")
    weight = _number(value["weight"], f"{path}.weight")
    if weight < 0.0:
        raise _error(f"{path}.weight", "must be greater than or equal to zero")
    return RandomOption(kind=kind, resource=resource, weight=weight)


def _node_from_document(value: Any, path: str) -> Node:
    if not isinstance(value, dict):
        raise _error(path, "node must be an object")
    unknown = set(value) - _NODE_FIELDS
    if unknown:
        raise _error(path, f"unknown field(s): {', '.join(sorted(unknown))}")
    if "kind" not in value:
        raise _error(path, "node requires field 'kind'")
    kind = value["kind"]
    if not isinstance(kind, str) or kind not in _KINDS:
        raise _coded_error(
            "MH_E_UNSUPPORTED_NODE_KIND", f"{path}.kind",
            f"unsupported node kind {kind!r}")
    if kind in _RESOURCE_KINDS:
        if "resource" not in value:
            raise _error(path, f"{kind} requires field 'resource'")
        resource = _token(value["resource"], f"{path}.resource")
    else:
        if "resource" in value:
            raise _error(path, f"{kind} forbids field 'resource'")
        resource = None
    name = value.get("name")
    if "name" in value and (not isinstance(name, str) or not name):
        raise _error(f"{path}.name", "must be a non-empty string")
    profile = (
        _token(value["profile"], f"{path}.profile")
        if "profile" in value else None)
    placement = (
        _inline_placement_from_document(value["placement"], f"{path}.placement")
        if "placement" in value else None)
    if profile is not None and placement is not None:
        raise _error(
            path,
            "fields 'profile' and 'placement' are mutually exclusive")
    transform = (
        _transform(value["transform"], f"{path}.transform", reader=True)
        if "transform" in value else IDENTITY_TRANSFORM)
    place_type = (
        _place_type(value["place_type"], f"{path}.place_type")
        if "place_type" in value else None)
    appearance_seed_boundary = (
        _appearance_seed_boundary(
            value["appearance_seed_boundary"],
            f"{path}.appearance_seed_boundary")
        if "appearance_seed_boundary" in value else False)
    if kind == "random":
        if "options" not in value or not isinstance(value["options"], list):
            raise _error(f"{path}.options", "random requires a non-empty array")
        if not value["options"]:
            raise _error(f"{path}.options", "random requires a non-empty array")
        options = [
            _option_from_document(option, f"{path}.options[{index}]")
            for index, option in enumerate(value["options"])]
        if not any(option.weight > 0.0 for option in options):
            raise _error(f"{path}.options", "at least one option weight must be positive")
    else:
        if "options" in value:
            raise _error(path, f"{kind} forbids field 'options'")
        options = []
    children_value = value.get("children", [])
    if not isinstance(children_value, list):
        raise _error(f"{path}.children", "must be an array")
    children = [
        _node_from_document(child, f"{path}.children[{index}]")
        for index, child in enumerate(children_value)]
    return Node(
        kind, transform, name, resource, profile, options, children,
        place_type, appearance_seed_boundary, placement=placement)


def _option_document(option: RandomOption, path: str) -> dict[str, Any]:
    if not isinstance(option, RandomOption):
        raise _error(path, "must be a RandomOption")
    if option.kind not in _OPTION_KINDS:
        raise _coded_error(
            "MH_E_UNSUPPORTED_NODE_KIND", f"{path}.kind",
            f"unsupported random option kind {option.kind!r}")
    document: dict[str, Any] = {"kind": option.kind}
    if option.kind == "empty":
        if option.resource is not None:
            raise _error(path, "empty option forbids resource")
    else:
        document["resource"] = _token(option.resource, f"{path}.resource")
    weight = _number(option.weight, f"{path}.weight")
    if weight < 0.0:
        raise _error(f"{path}.weight", "must be greater than or equal to zero")
    document["weight"] = weight
    return document


def _node_document(node: Node, path: str) -> dict[str, Any]:
    if not isinstance(node, Node):
        raise _error(path, "must be a Node")
    if node.kind not in _KINDS:
        raise _coded_error(
            "MH_E_UNSUPPORTED_NODE_KIND", f"{path}.kind",
            f"unsupported node kind {node.kind!r}")
    document: dict[str, Any] = {"kind": node.kind}
    if node.kind in _RESOURCE_KINDS:
        document["resource"] = _token(node.resource, f"{path}.resource")
    elif node.resource is not None:
        raise _error(path, f"{node.kind} forbids resource")
    if node.name is not None:
        if not isinstance(node.name, str) or not node.name:
            raise _error(f"{path}.name", "must be a non-empty string")
        document["name"] = node.name
    if not isinstance(node.transform, CompositeTransform):
        raise _error(f"{path}.transform", "must be CompositeTransform")
    canonical_transform = _transform(
        node.transform.disk_dict(), f"{path}.transform", reader=False)
    transform_document: dict[str, Any] = {}
    if canonical_transform.translation_cm != _IDENTITY_TRANSLATION:
        transform_document["translation_cm"] = list(canonical_transform.translation_cm)
    if canonical_transform.rotation_quat != _IDENTITY_ROTATION:
        transform_document["rotation_quat"] = list(canonical_transform.rotation_quat)
    if canonical_transform.scale != _IDENTITY_SCALE:
        transform_document["scale"] = list(canonical_transform.scale)
    if transform_document:
        document["transform"] = transform_document
    if node.profile is not None:
        if node.placement is not None:
            raise _error(
                path,
                "fields 'profile' and 'placement' are mutually exclusive")
        document["profile"] = _token(node.profile, f"{path}.profile")
    if node.placement is not None:
        document["placement"] = _inline_placement_document(
            node.placement, f"{path}.placement")
    # OPEN-V5-21 order extended by the 2026-08-31 owner revision of
    # OPEN-V5-15: kind, resource, name, transform, profile, placement,
    # place_type, appearance_seed_boundary, options, children.  Every carrier
    # is elided at its default so existing documents stay byte identical.
    if node.place_type is not None:
        document["place_type"] = _place_type(node.place_type, f"{path}.place_type")
    if _appearance_seed_boundary(
            node.appearance_seed_boundary, f"{path}.appearance_seed_boundary"):
        document["appearance_seed_boundary"] = True
    if node.kind == "random":
        if not isinstance(node.options, list) or not node.options:
            raise _error(f"{path}.options", "random requires a non-empty list")
        options = [
            _option_document(option, f"{path}.options[{index}]")
            for index, option in enumerate(node.options)]
        if not any(option["weight"] > 0.0 for option in options):
            raise _error(f"{path}.options", "at least one option weight must be positive")
        document["options"] = options
    elif node.options:
        raise _error(path, f"{node.kind} forbids options")
    if not isinstance(node.children, list):
        raise _error(f"{path}.children", "must be a list")
    if node.children:
        document["children"] = [
            _node_document(child, f"{path}.children[{index}]")
            for index, child in enumerate(node.children)]
    return document


def composite_document(composite: Composite) -> dict[str, Any]:
    """Validate one DTO and return its insertion-ordered v5 disk document."""
    if not isinstance(composite, Composite):
        raise TypeError("composite must be Composite")
    if not isinstance(composite.nodes, list):
        raise _error("nodes", "must be a list")
    return {
        "v": 5,
        "nodes": [
            _node_document(node, f"nodes[{index}]")
            for index, node in enumerate(composite.nodes)],
    }


def composite_json_bytes(composite: Composite | dict[str, Any]) -> bytes:
    """Return the canonical UTF-8/LF Source Protocol v5 byte form."""
    if isinstance(composite, dict):
        composite = parse_composite(composite)
    return canonical_json_bytes(composite_document(composite))


def _parse_json(value: str | bytes) -> Any:
    try:
        return parse_json(value)
    except CanonicalJSONDuplicateKey as exc:
        raise _error(exc.key, "duplicate JSON field") from exc
    except CanonicalJSONNonFinite as exc:
        raise _coded_error(
            "MH_E_NAN_INF_VALUE", "$", f"invalid number {exc.token}") from exc
    except CanonicalJSONSyntaxError as exc:
        raise _error("$", f"invalid JSON: {exc}") from exc


def parse_composite(
    value: dict[str, Any] | str | bytes,
    *,
    name: str = "",
) -> Composite:
    """Parse the closed v5 grammar without legacy fallback or repair."""
    document = _parse_json(value) if isinstance(value, (str, bytes)) else value
    if not isinstance(document, dict):
        raise _error("$", "composite payload must be an object")
    if "v" not in document:
        raise _coded_error(
            "MH_E_COMPOSITE_LEGACY_GENERATION", "$.v", _LEGACY_MESSAGE)
    version = document["v"]
    if isinstance(version, bool) or not isinstance(version, int) or version != 5:
        raise _coded_error(
            "MH_E_UNKNOWN_SCHEMA_VERSION", "$.v",
            f"composite schema version must be integer 5, got {version!r}")
    if next(iter(document)) != "v":
        raise _error("$", "field 'v' must be the first root field")
    if set(document) != {"v", "nodes"}:
        unknown = set(document) - {"v", "nodes"}
        if unknown:
            raise _error("$", f"unknown field(s): {', '.join(sorted(unknown))}")
        raise _error("$", "root requires exactly fields 'v' and 'nodes'")
    nodes_value = document["nodes"]
    if not isinstance(nodes_value, list):
        raise _error("nodes", "must be an array")
    return Composite(
        name,
        [_node_from_document(node, f"nodes[{index}]")
         for index, node in enumerate(nodes_value)],
    )


def read_composite_file(path: str | Path) -> Composite:
    """Read one ``<name>.composite`` file and preserve filename identity."""
    source = Path(path)
    if source.suffix != ".composite":
        raise _coded_error(
            "MH_E_NONCANONICAL_RESOURCE_NAME", str(source),
            "composite filename must end with exact lowercase .composite")
    if _TOKEN_RE.fullmatch(source.stem) is None:
        raise _coded_error(
            "MH_E_NONCANONICAL_RESOURCE_NAME", str(source),
            "composite filename stem must match [a-z0-9_]+ exactly")
    return parse_composite(source.read_bytes(), name=source.stem)


def _walk_nodes(nodes: Iterable[Node]) -> Iterable[Node]:
    for node in nodes:
        yield node
        yield from _walk_nodes(node.children)


def iter_profile_references(composite: Composite) -> Iterable[str]:
    """Yield every placement-profile token in significant DFS node order."""
    for node in _walk_nodes(composite.nodes):
        if node.profile is not None:
            yield node.profile


def iter_resource_references(
    composite: Composite,
    *,
    kind: str | None = None,
) -> Iterable[str]:
    """Yield ordinary and all-option references in significant DFS order."""
    if kind is not None and kind not in _RESOURCE_KINDS:
        raise ValueError(f"invalid resource kind {kind!r}")
    for node in _walk_nodes(composite.nodes):
        if node.resource is not None and (kind is None or node.kind == kind):
            yield node.resource
        for option in node.options:
            if option.resource is not None and (kind is None or option.kind == kind):
                yield option.resource


def validate_composite_cycles(
    root_name: str,
    resolver: Mapping[str, Composite | None] | Callable[[str], Composite | None],
) -> None:
    """Fail closed on cycles through ordinary and every random option edge."""
    _token(root_name, "root")
    resolve = resolver.get if isinstance(resolver, Mapping) else resolver
    active: list[str] = []
    complete: set[str] = set()

    def visit(name: str) -> None:
        if name in active:
            cycle = " -> ".join(active[active.index(name):] + [name])
            raise _coded_error(
                "MH_E_COMPOSITE_CYCLE", name,
                f"composite dependency cycle: {cycle}")
        if name in complete:
            return
        composite = resolve(name)
        if composite is None:
            raise _coded_error(
                "MH_E_UNRESOLVED_COMPOSITE_REFERENCE", name,
                "composite resource cannot be resolved")
        if not isinstance(composite, Composite):
            raise TypeError("composite resolver must return Composite or None")
        active.append(name)
        for dependency in iter_resource_references(composite, kind="composite"):
            visit(dependency)
        active.pop()
        complete.add(name)

    visit(root_name)
