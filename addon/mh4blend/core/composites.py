"""Pure Source Protocol v4 composite codec and dependency gates."""

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
from .model import Composite, CompositeTransform, IDENTITY_TRANSFORM, Node

__all__ = [
    "CompositeValueError",
    "composite_document",
    "composite_json_bytes",
    "iter_resource_references",
    "parse_composite",
    "read_composite_file",
    "validate_composite_cycles",
]


_TOKEN_RE = re.compile(r"^[a-z0-9_]+$")
_KINDS = frozenset({"mesh", "actor", "composite", "group"})
_NODE_FIELDS = frozenset({"kind", "resource", "name", "transform", "children"})
_TRANSFORM_FIELDS = frozenset({"translation_cm", "rotation_quat", "scale"})
_IDENTITY_TRANSLATION = (0.0, 0.0, 0.0)
_IDENTITY_ROTATION = (0.0, 0.0, 0.0, 1.0)
_IDENTITY_SCALE = (1.0, 1.0, 1.0)
_QUATERNION_NORM_TOLERANCE = 1.0e-3


class CompositeValueError(ValueError):
    """A composite cannot be represented without losing v4 semantics."""

    def __init__(self, code: str, path: str, message: str):
        self.code = code
        self.path = path
        super().__init__(f"{code}: {path}: {message}")


def _error(path: str, message: str) -> CompositeValueError:
    return CompositeValueError("MH_E_COMPOSITE_GRAMMAR", path, message)


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
        raise CompositeValueError(
            "MH_E_NAN_INF_VALUE", path, "NaN/Inf is not a composite value")
    try:
        return narrow_float32(value)
    except (TypeError, ValueError) as exc:
        raise _error(path, "must be representable as finite IEEE float32") from exc


def _vector(value: Any, length: int, path: str) -> tuple[float, ...]:
    if not isinstance(value, list) or len(value) != length:
        raise _error(path, f"must contain exactly {length} numbers")
    return tuple(_number(component, f"{path}[{index}]")
                 for index, component in enumerate(value))


def _canonical_quaternion(
        value: Any, path: str, *, require_unit: bool) -> tuple[float, ...]:
    components = _vector(value, 4, path)
    norm = math.sqrt(sum(component * component for component in components))
    if norm == 0.0 or (require_unit and abs(norm - 1.0) > _QUATERNION_NORM_TOLERANCE):
        raise _error(path, "quaternion norm must be within 1e-3 of one")

    normalized = tuple(narrow_float32(component / norm) for component in components)
    # Recompute only to protect an exotic underflow after binary32 narrowing.
    narrowed_norm = math.sqrt(sum(component * component for component in normalized))
    if narrowed_norm == 0.0:
        raise _error(path, "quaternion must be nonzero")

    negate = normalized[3] < 0.0
    if normalized[3] == 0.0:
        first_nonzero = next((component for component in normalized[:3]
                              if component != 0.0), 0.0)
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
            value["rotation_quat"], f"{path}.rotation_quat",
            require_unit=reader)
        if "rotation_quat" in value else _IDENTITY_ROTATION)
    scale = (
        _vector(value["scale"], 3, f"{path}.scale")
        if "scale" in value else _IDENTITY_SCALE)
    if any(component <= 0.0 for component in scale):
        raise CompositeValueError(
            "MH_E_INVALID_SCALE", f"{path}.scale",
            "all scale components must be greater than zero")
    return CompositeTransform(translation, rotation, scale)


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
        raise CompositeValueError(
            "MH_E_UNSUPPORTED_NODE_KIND", f"{path}.kind",
            f"unsupported node kind {kind!r}")

    has_resource = "resource" in value
    if kind == "group":
        if has_resource:
            raise _error(path, "group forbids field 'resource'")
        resource = None
    else:
        if not has_resource:
            raise _error(path, f"{kind} requires field 'resource'")
        resource = _token(value["resource"], f"{path}.resource")

    name = value.get("name")
    if "name" in value and (not isinstance(name, str) or not name):
        raise _error(f"{path}.name", "must be a non-empty string")
    transform = (
        _transform(value["transform"], f"{path}.transform", reader=True)
        if "transform" in value else IDENTITY_TRANSFORM)
    children_value = value.get("children", [])
    if not isinstance(children_value, list):
        raise _error(f"{path}.children", "must be an array")
    children = [
        _node_from_document(child, f"{path}.children[{index}]")
        for index, child in enumerate(children_value)
    ]
    return Node(kind, transform, name, resource, children)


def _node_document(node: Node, path: str) -> dict[str, Any]:
    if not isinstance(node, Node):
        raise _error(path, "must be a Node")
    if node.kind not in _KINDS:
        raise CompositeValueError(
            "MH_E_UNSUPPORTED_NODE_KIND", f"{path}.kind",
            f"unsupported node kind {node.kind!r}")

    document: dict[str, Any] = {"kind": node.kind}
    if node.kind == "group":
        if node.resource is not None:
            raise _error(path, "group forbids resource")
    else:
        document["resource"] = _token(node.resource, f"{path}.resource")
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
        transform_document["translation_cm"] = list(
            canonical_transform.translation_cm)
    if canonical_transform.rotation_quat != _IDENTITY_ROTATION:
        transform_document["rotation_quat"] = list(
            canonical_transform.rotation_quat)
    if canonical_transform.scale != _IDENTITY_SCALE:
        transform_document["scale"] = list(canonical_transform.scale)
    if transform_document:
        document["transform"] = transform_document

    if not isinstance(node.children, list):
        raise _error(f"{path}.children", "must be a list")
    if node.children:
        document["children"] = [
            _node_document(child, f"{path}.children[{index}]")
            for index, child in enumerate(node.children)
        ]
    return document


def composite_document(composite: Composite) -> dict[str, Any]:
    """Validate one DTO and return its insertion-ordered disk document."""
    if not isinstance(composite, Composite):
        raise TypeError("composite must be Composite")
    if not isinstance(composite.nodes, list):
        raise _error("nodes", "must be a list")
    return {"nodes": [
        _node_document(node, f"nodes[{index}]")
        for index, node in enumerate(composite.nodes)
    ]}


def composite_json_bytes(composite: Composite | dict[str, Any]) -> bytes:
    """Return the canonical UTF-8/LF byte form from docs/08 section 6."""
    if isinstance(composite, dict):
        composite = parse_composite(composite)
    return canonical_json_bytes(composite_document(composite))


def _parse_json(value: str | bytes) -> Any:
    try:
        return parse_json(value)
    except CanonicalJSONDuplicateKey as exc:
        raise _error(exc.key, "duplicate JSON field") from exc
    except CanonicalJSONNonFinite as exc:
        raise CompositeValueError(
            "MH_E_NAN_INF_VALUE", "$", f"invalid number {exc.token}") from exc
    except CanonicalJSONSyntaxError as exc:
        raise _error("$", f"invalid JSON: {exc}") from exc


def parse_composite(
        value: dict[str, Any] | str | bytes, *, name: str = "") -> Composite:
    """Parse the closed composite grammar without fallback or repair."""
    document = _parse_json(value) if isinstance(value, (str, bytes)) else value
    if not isinstance(document, dict):
        raise _error("$", "composite payload must be an object")
    if set(document) != {"nodes"}:
        unknown = set(document) - {"nodes"}
        if unknown:
            raise _error("$", f"unknown field(s): {', '.join(sorted(unknown))}")
        raise _error("$", "root requires exactly field 'nodes'")
    nodes_value = document["nodes"]
    if not isinstance(nodes_value, list):
        raise _error("nodes", "must be an array")
    return Composite(name, [
        _node_from_document(node, f"nodes[{index}]")
        for index, node in enumerate(nodes_value)
    ])


def read_composite_file(path: str | Path) -> Composite:
    """Read one ``<name>.composite`` file and preserve filename identity."""
    source = Path(path)
    if source.suffix != ".composite":
        raise CompositeValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME", str(source),
            "composite filename must end with exact lowercase .composite")
    if _TOKEN_RE.fullmatch(source.stem) is None:
        raise CompositeValueError(
            "MH_E_NONCANONICAL_RESOURCE_NAME", str(source),
            "composite filename stem must match [a-z0-9_]+ exactly")
    return parse_composite(source.read_bytes(), name=source.stem)


def _walk_nodes(nodes: Iterable[Node]) -> Iterable[Node]:
    for node in nodes:
        yield node
        yield from _walk_nodes(node.children)


def iter_resource_references(
        composite: Composite, *, kind: str | None = None) -> Iterable[str]:
    """Yield resource tokens depth-first in significant node order."""
    if kind is not None and kind not in _KINDS - {"group"}:
        raise ValueError(f"invalid resource kind {kind!r}")
    for node in _walk_nodes(composite.nodes):
        if node.resource is not None and (kind is None or node.kind == kind):
            yield node.resource


def validate_composite_cycles(
        root_name: str,
        resolver: Mapping[str, Composite | None]
        | Callable[[str], Composite | None]) -> None:
    """Fail closed on self/ancestor inclusion in the composite graph.

    Only composite edges participate.  Mesh and actor availability belongs to
    the host resolver because Blender intentionally has no actor registry.
    """
    _token(root_name, "root")
    resolve = resolver.get if isinstance(resolver, Mapping) else resolver
    active: list[str] = []
    complete: set[str] = set()

    def visit(name: str) -> None:
        if name in active:
            cycle = " -> ".join(active[active.index(name):] + [name])
            raise CompositeValueError(
                "MH_E_COMPOSITE_CYCLE", name,
                f"composite dependency cycle: {cycle}")
        if name in complete:
            return
        composite = resolve(name)
        if composite is None:
            raise CompositeValueError(
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
