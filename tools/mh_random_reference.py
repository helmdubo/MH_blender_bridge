"""Bpy-free Source Protocol v5 random reference implementation.

This module is the executable reference for ``mh.random_stream:1``.  It is
deliberately outside the Blender extension: placement seeds and resolution are
not Blender authoring state.  The implementation follows docs/10 sections
13.1--13.3 and 13.8 and exposes immutable graph, closure, trace, and plan values
for the shared Python/C++ golden vectors.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import math
from typing import Mapping, Sequence

from blake3 import blake3

from addon.mh4blend.core.canonical_json import (
    canonical_json_bytes,
    narrow_float32,
)


RANDOM_STREAM_TAG = "mh.random_stream:1"
RESOLVER_TAG = "mh.random_resolver:2"

_MASK64 = (1 << 64) - 1
_INT32_MIN = -(1 << 31)
_INT32_MAX = (1 << 31) - 1
_UINT32_SCALE = 2.0 ** -32
_SPLITMIX_GAMMA = 0x9E3779B97F4A7C15
_SPLITMIX_MUL1 = 0xBF58476D1CE4E5B9
_SPLITMIX_MUL2 = 0x94D049BB133111EB
_RESOURCE_KINDS = frozenset({
    "composite",
    "material",
    "placement_profile",
    "static_mesh",
    "texture",
})
_NODE_KINDS = frozenset({"mesh", "actor", "composite", "group", "random"})
_OPTION_KINDS = frozenset({"mesh", "actor", "composite", "empty"})


class RandomReferenceError(ValueError):
    """Fail-closed reference input or resolution error."""


def _f32(value: int | float) -> float:
    try:
        return narrow_float32(value)
    except (TypeError, ValueError) as exc:
        raise RandomReferenceError(str(exc)) from exc


def _tuple_f32(values: Sequence[int | float], size: int, label: str) -> tuple[float, ...]:
    if len(values) != size:
        raise RandomReferenceError(f"{label} must contain exactly {size} values")
    return tuple(_f32(value) for value in values)


def _canonical_quaternion(values: Sequence[int | float]) -> tuple[float, float, float, float]:
    quat = _tuple_f32(values, 4, "rotation_quat")
    norm = math.sqrt(sum(float(component) * float(component) for component in quat))
    if not math.isfinite(norm) or norm == 0.0:
        raise RandomReferenceError("rotation_quat must have finite non-zero length")
    result = tuple(_f32(component / norm) for component in quat)
    negate = result[3] < 0.0
    if result[3] == 0.0:
        first_nonzero = next(
            (component for component in result[:3] if component != 0.0),
            0.0,
        )
        negate = first_nonzero < 0.0
    if negate:
        result = tuple(_f32(-component) for component in result)
    return result


def _quat_multiply(
    left: Sequence[int | float],
    right: Sequence[int | float],
) -> tuple[float, float, float, float]:
    lx, ly, lz, lw = (float(value) for value in left)
    rx, ry, rz, rw = (float(value) for value in right)
    return _canonical_quaternion((
        lw * rx + lx * rw + ly * rz - lz * ry,
        lw * ry - lx * rz + ly * rw + lz * rx,
        lw * rz + lx * ry - ly * rx + lz * rw,
        lw * rw - lx * rx - ly * ry - lz * rz,
    ))


def _rotate_vector(
    quat: Sequence[int | float],
    vector: Sequence[int | float],
) -> tuple[float, float, float]:
    qx, qy, qz, qw = (float(value) for value in quat)
    vx, vy, vz = (float(value) for value in vector)
    tx = 2.0 * (qy * vz - qz * vy)
    ty = 2.0 * (qz * vx - qx * vz)
    tz = 2.0 * (qx * vy - qy * vx)
    return (
        _f32(vx + qw * tx + qy * tz - qz * ty),
        _f32(vy + qw * ty + qz * tx - qx * tz),
        _f32(vz + qw * tz + qx * ty - qy * tx),
    )


def _axis_quaternion(axis: int, degrees: float) -> tuple[float, float, float, float]:
    half_angle = math.radians(float(degrees)) * 0.5
    sine = math.sin(half_angle)
    cosine = math.cos(half_angle)
    components = [0.0, 0.0, 0.0, cosine]
    components[axis] = sine
    return _canonical_quaternion(components)


def _rotation_sample_quaternion(
    degrees_xyz: Sequence[int | float],
) -> tuple[float, float, float, float]:
    x, y, z = degrees_xyz
    qx = _axis_quaternion(0, float(x))
    qy = _axis_quaternion(1, float(y))
    qz = _axis_quaternion(2, float(z))
    return _quat_multiply(_quat_multiply(qz, qy), qx)


def _blake3_160(payload: bytes) -> str:
    return "blake3-160:" + blake3(payload).digest(length=20).hex()


def raw_payload_hash(payload: bytes) -> str:
    """Return one self-describing BLAKE3-160 raw payload hash."""
    if not isinstance(payload, bytes):
        raise TypeError("payload must be bytes")
    return _blake3_160(payload)


def _validate_hash(value: str) -> str:
    prefix = "blake3-160:"
    if not isinstance(value, str) or not value.startswith(prefix):
        raise RandomReferenceError(f"invalid BLAKE3-160 hash {value!r}")
    digits = value[len(prefix):]
    if len(digits) != 40 or any(character not in "0123456789abcdef" for character in digits):
        raise RandomReferenceError(f"invalid BLAKE3-160 hash {value!r}")
    return value


def _splitmix64_step(state: int) -> tuple[int, int]:
    state = (state + _SPLITMIX_GAMMA) & _MASK64
    value = state
    value = ((value ^ (value >> 30)) * _SPLITMIX_MUL1) & _MASK64
    value = ((value ^ (value >> 27)) * _SPLITMIX_MUL2) & _MASK64
    value ^= value >> 31
    return state, value & _MASK64


def placement_state(seed: int) -> int:
    """Return the frozen §13.1 initial state for one int32 placement seed."""
    if isinstance(seed, bool) or not isinstance(seed, int):
        raise RandomReferenceError("seed must be an int32")
    if seed < _INT32_MIN or seed > _INT32_MAX:
        raise RandomReferenceError("seed must be an int32")
    _, initial_state = _splitmix64_step(seed & 0xFFFFFFFF)
    return initial_state


def path_hash64(node_path: str) -> int:
    """Hash one canonical NodePath per §13.8 (BLAKE3 prefix, little-endian)."""
    if not isinstance(node_path, str) or not node_path:
        raise RandomReferenceError("NodePath must be a non-empty string")
    prefix = blake3(node_path.encode("utf-8")).digest()[:8]
    return int.from_bytes(prefix, byteorder="little", signed=False)


class RandomStream:
    """Mutable uint64 stream with the frozen ``mh.random_stream:1`` bytes."""

    def __init__(self, seed: int):
        initial_state = placement_state(seed)
        self._state = initial_state
        self._initial_state = initial_state

    @classmethod
    def _from_initial_state(cls, initial_state: int) -> "RandomStream":
        stream = cls.__new__(cls)
        stream._state = initial_state & _MASK64
        stream._initial_state = stream._state
        return stream

    @property
    def state(self) -> int:
        return self._state

    @property
    def initial_state(self) -> int:
        return self._initial_state

    def next_u64(self) -> int:
        self._state, value = _splitmix64_step(self._state)
        return value

    def next_u32(self) -> int:
        return self.next_u64() >> 32

    def next_unit(self) -> float:
        return self.next_u32() * _UINT32_SCALE


def node_random_stream(seed: int, node_path: str) -> RandomStream:
    """Open the independent §13.8 stream for one canonical NodePath."""
    mixed_state = placement_state(seed) ^ path_hash64(node_path)
    _, initial_state = _splitmix64_step(mixed_state)
    return RandomStream._from_initial_state(initial_state)


@dataclass(frozen=True, order=True)
class ResourceKey:
    kind: str
    name: str

    def __post_init__(self) -> None:
        if self.kind not in _RESOURCE_KINDS:
            raise RandomReferenceError(f"unsupported source resource kind {self.kind!r}")
        if not self.name:
            raise RandomReferenceError("resource name must be non-empty")

    def __str__(self) -> str:
        return f"{self.kind}:{self.name}"


@dataclass(frozen=True)
class TRS:
    translation_cm: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation_quat: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0)

    def __post_init__(self) -> None:
        translation = _tuple_f32(self.translation_cm, 3, "translation_cm")
        rotation = _canonical_quaternion(self.rotation_quat)
        scale = _tuple_f32(self.scale, 3, "scale")
        if any(component == 0.0 for component in scale):
            raise RandomReferenceError("scale components must be non-zero")
        object.__setattr__(self, "translation_cm", translation)
        object.__setattr__(self, "rotation_quat", rotation)
        object.__setattr__(self, "scale", scale)

    def signature_document(self) -> dict:
        return {
            "translation_cm": list(self.translation_cm),
            "rotation_quat": list(self.rotation_quat),
            "scale": list(self.scale),
        }


IDENTITY_TRS = TRS()


def compose_trs(parent: TRS, local: TRS) -> TRS:
    """Compose representable parent/local T/R/S without matrix approximation."""
    scaled_local_translation = tuple(
        float(parent.scale[index]) * float(local.translation_cm[index])
        for index in range(3)
    )
    rotated_local_translation = _rotate_vector(
        parent.rotation_quat,
        scaled_local_translation,
    )
    return TRS(
        translation_cm=tuple(
            _f32(float(parent.translation_cm[index]) + rotated_local_translation[index])
            for index in range(3)
        ),
        rotation_quat=_quat_multiply(parent.rotation_quat, local.rotation_quat),
        scale=tuple(
            _f32(float(parent.scale[index]) * float(local.scale[index]))
            for index in range(3)
        ),
    )


@dataclass(frozen=True)
class Range:
    base: float
    deviation: float

    def __post_init__(self) -> None:
        base = _f32(self.base)
        deviation = _f32(self.deviation)
        if deviation < 0.0:
            raise RandomReferenceError("profile deviation must be non-negative")
        object.__setattr__(self, "base", base)
        object.__setattr__(self, "deviation", deviation)


@dataclass(frozen=True)
class PlacementProfile:
    name: str
    offset_cm: tuple[Range, Range, Range] | None = None
    rotation_deg: tuple[Range, Range, Range] | None = None
    uniform_scale: Range | None = None
    vertical_scale: Range | None = None

    def __post_init__(self) -> None:
        if not self.name:
            raise RandomReferenceError("profile name must be non-empty")
        for label, ranges in (
            ("offset_cm", self.offset_cm),
            ("rotation_deg", self.rotation_deg),
        ):
            if ranges is not None and len(ranges) != 3:
                raise RandomReferenceError(f"{label} must contain exactly three ranges")
        for label, value_range in (
            ("uniform_scale", self.uniform_scale),
            ("vertical_scale", self.vertical_scale),
        ):
            if value_range is not None and value_range.base - value_range.deviation <= 0.0:
                raise RandomReferenceError(f"{label} range must remain strictly positive")


@dataclass(frozen=True)
class RandomOption:
    kind: str
    weight: float
    resource: str | None = None

    def __post_init__(self) -> None:
        if self.kind not in _OPTION_KINDS:
            raise RandomReferenceError(f"unsupported random option kind {self.kind!r}")
        weight = _f32(self.weight)
        if weight < 0.0:
            raise RandomReferenceError("random option weight must be non-negative")
        if self.kind == "empty":
            if self.resource is not None:
                raise RandomReferenceError("empty option must not have a resource")
        elif not self.resource:
            raise RandomReferenceError(f"{self.kind} option requires a resource")
        object.__setattr__(self, "weight", weight)


@dataclass(frozen=True)
class Node:
    kind: str
    resource: str | None = None
    transform: TRS = IDENTITY_TRS
    profile: str | None = None
    options: tuple[RandomOption, ...] = field(default_factory=tuple)
    children: tuple["Node", ...] = field(default_factory=tuple)

    def __post_init__(self) -> None:
        if self.kind not in _NODE_KINDS:
            raise RandomReferenceError(f"unsupported node kind {self.kind!r}")
        if self.kind in {"mesh", "actor", "composite"}:
            if not self.resource:
                raise RandomReferenceError(f"{self.kind} node requires a resource")
        elif self.resource is not None:
            raise RandomReferenceError(f"{self.kind} node must not have a resource")
        if self.kind == "random":
            if not self.options:
                raise RandomReferenceError("random node requires options")
            if not any(option.weight > 0.0 for option in self.options):
                raise RandomReferenceError("random node requires a positive option weight")
        elif self.options:
            raise RandomReferenceError(f"{self.kind} node must not have options")
        object.__setattr__(self, "options", tuple(self.options))
        object.__setattr__(self, "children", tuple(self.children))


@dataclass(frozen=True)
class Composite:
    name: str
    nodes: tuple[Node, ...] = field(default_factory=tuple)

    def __post_init__(self) -> None:
        if not self.name:
            raise RandomReferenceError("composite name must be non-empty")
        object.__setattr__(self, "nodes", tuple(self.nodes))


@dataclass(frozen=True)
class SourceClosure:
    resources: tuple[ResourceKey, ...]
    raw_hashes: tuple[tuple[ResourceKey, str], ...]
    hash_preimage: bytes
    closure_hash: str


@dataclass(frozen=True)
class SelectionDecision:
    path: str
    option: int
    weights: tuple[float, ...]
    total: float
    raw_u32: int
    unit: float
    target: float

    def signature_document(self) -> dict:
        return {
            "path": self.path,
            "option": self.option,
            "total": _f32(self.total),
            "draw": self.raw_u32,
        }


@dataclass(frozen=True)
class DrawTraceEntry:
    path: str
    role: str
    raw_u32: int
    unit: float
    sample: float


@dataclass(frozen=True)
class ResolvedLeaf:
    kind: str
    resource: str
    world_trs: TRS
    origin: str

    def signature_document(self) -> dict:
        return {
            "kind": self.kind,
            "resource": self.resource,
            "trs": self.world_trs.signature_document(),
        }


@dataclass(frozen=True)
class ResolvedPlan:
    seed: int
    closure: SourceClosure
    decisions: tuple[SelectionDecision, ...]
    draws: tuple[DrawTraceEntry, ...]
    leaves: tuple[ResolvedLeaf, ...]
    selected_dependencies: tuple[str, ...]
    signature_preimage: bytes
    resolved_signature: str


def _node_source_dependencies(node: Node) -> tuple[ResourceKey, ...]:
    dependencies: list[ResourceKey] = []
    if node.profile is not None:
        dependencies.append(ResourceKey("placement_profile", node.profile))
    if node.kind == "mesh":
        dependencies.append(ResourceKey("static_mesh", node.resource or ""))
    elif node.kind == "composite":
        dependencies.append(ResourceKey("composite", node.resource or ""))
    elif node.kind == "random":
        for option in node.options:
            if option.kind == "mesh":
                dependencies.append(ResourceKey("static_mesh", option.resource or ""))
            elif option.kind == "composite":
                dependencies.append(ResourceKey("composite", option.resource or ""))
    return tuple(dependencies)


def build_source_closure(
    root: str,
    composites: Mapping[str, Composite],
    profiles: Mapping[str, PlacementProfile],
    raw_hashes: Mapping[ResourceKey, str],
    resource_dependencies: Mapping[ResourceKey, Sequence[ResourceKey]] | None = None,
) -> SourceClosure:
    """Build the seed-free all-options source closure and reject all cycles."""
    dependencies = resource_dependencies or {}
    resources: set[ResourceKey] = set()
    visiting: list[str] = []
    visited: set[str] = set()
    dependency_visiting: list[ResourceKey] = []
    dependency_visited: set[ResourceKey] = set()

    def visit_declared_dependencies(key: ResourceKey) -> None:
        if key in dependency_visiting:
            chain = " -> ".join(str(value) for value in (*dependency_visiting, key))
            raise RandomReferenceError(f"source dependency cycle: {chain}")
        if key in dependency_visited:
            return
        dependency_visiting.append(key)
        for dependency in sorted(tuple(dependencies.get(key, ())), key=str):
            if not isinstance(dependency, ResourceKey):
                raise RandomReferenceError(
                    f"source dependency of {key} must be a ResourceKey")
            resources.add(dependency)
            if (dependency.kind == "placement_profile" and
                    dependency.name not in profiles):
                raise RandomReferenceError(f"missing {dependency}")
            visit_declared_dependencies(dependency)
            if dependency.kind == "composite":
                visit_composite(dependency.name)
        dependency_visiting.pop()
        dependency_visited.add(key)

    def visit_composite(name: str) -> None:
        if name in visiting:
            chain = " -> ".join((*visiting, name))
            raise RandomReferenceError(f"composite cycle: {chain}")
        if name in visited:
            return
        composite = composites.get(name)
        if composite is None:
            raise RandomReferenceError(f"missing composite:{name}")
        key = ResourceKey("composite", name)
        resources.add(key)
        visiting.append(name)
        visit_declared_dependencies(key)

        def visit_node(node: Node) -> None:
            for dependency in _node_source_dependencies(node):
                resources.add(dependency)
                if dependency.kind == "placement_profile" and dependency.name not in profiles:
                    raise RandomReferenceError(f"missing {dependency}")
                visit_declared_dependencies(dependency)
                if dependency.kind == "composite":
                    visit_composite(dependency.name)
            for child in node.children:
                visit_node(child)

        for node in composite.nodes:
            visit_node(node)
        visiting.pop()
        visited.add(name)

    visit_composite(root)
    ordered = tuple(sorted(resources, key=str))
    ordered_hashes: list[tuple[ResourceKey, str]] = []
    for key in ordered:
        if key not in raw_hashes:
            raise RandomReferenceError(f"missing raw payload hash for {key}")
        ordered_hashes.append((key, _validate_hash(raw_hashes[key])))
    preimage = b"".join(value.encode("ascii") for _, value in ordered_hashes)
    return SourceClosure(
        resources=ordered,
        raw_hashes=tuple(ordered_hashes),
        hash_preimage=preimage,
        closure_hash=_blake3_160(preimage),
    )


def select_weighted(
    stream: RandomStream,
    path: str,
    options: Sequence[RandomOption],
) -> SelectionDecision:
    total = 0.0
    weights: list[float] = []
    for option in options:
        weight = float(option.weight)
        weights.append(option.weight)
        total += weight
    if not math.isfinite(total) or total <= 0.0:
        raise RandomReferenceError(f"random node {path} has invalid total weight")
    raw = stream.next_u32()
    unit = raw * _UINT32_SCALE
    target = unit * total
    cumulative = 0.0
    for index, weight in enumerate(weights):
        cumulative += float(weight)
        if cumulative > target:
            return SelectionDecision(
                path=path,
                option=index,
                weights=tuple(weights),
                total=total,
                raw_u32=raw,
                unit=unit,
                target=target,
            )
    raise AssertionError("positive ordered weights did not select an option")


@dataclass(frozen=True)
class SampledProfile:
    offset_cm: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation_deg: tuple[float, float, float] = (0.0, 0.0, 0.0)
    uniform_scale: float = 1.0
    vertical_scale: float = 1.0


def _sample_range(
    stream: RandomStream,
    node_path: str,
    role: str,
    value_range: Range,
    trace: list[DrawTraceEntry],
) -> float:
    raw = stream.next_u32()
    unit = raw * _UINT32_SCALE
    sample = _f32(
        float(value_range.base) +
        (unit * 2.0 - 1.0) * float(value_range.deviation)
    )
    trace.append(DrawTraceEntry(
        path=node_path,
        role=role,
        raw_u32=raw,
        unit=unit,
        sample=sample,
    ))
    return sample


def _sample_profile(
    stream: RandomStream,
    node_path: str,
    profile: PlacementProfile,
    trace: list[DrawTraceEntry],
) -> SampledProfile:
    offset = (0.0, 0.0, 0.0)
    rotation = (0.0, 0.0, 0.0)
    uniform = 1.0
    vertical = 1.0
    if profile.offset_cm is not None:
        offset = tuple(
            _sample_range(stream, node_path, f"offset_{axis}", value_range, trace)
            for axis, value_range in zip("xyz", profile.offset_cm, strict=True)
        )
    if profile.rotation_deg is not None:
        rotation = tuple(
            _sample_range(stream, node_path, f"rotation_{axis}", value_range, trace)
            for axis, value_range in zip("xyz", profile.rotation_deg, strict=True)
        )
    if profile.uniform_scale is not None:
        uniform = _sample_range(
            stream, node_path, "uniform_scale", profile.uniform_scale, trace)
    if profile.vertical_scale is not None:
        vertical = _sample_range(
            stream, node_path, "vertical_scale", profile.vertical_scale, trace)
    return SampledProfile(
        offset_cm=offset,
        rotation_deg=rotation,
        uniform_scale=uniform,
        vertical_scale=vertical,
    )


def sample_placement_profile(
    stream: RandomStream,
    node_path: str,
    profile: PlacementProfile,
) -> tuple[SampledProfile, tuple[DrawTraceEntry, ...]]:
    """Sample one profile and return its exact draw trace."""
    trace: list[DrawTraceEntry] = []
    sample = _sample_profile(stream, node_path, profile, trace)
    return sample, tuple(trace)


def _apply_profile(authored: TRS, sample: SampledProfile) -> TRS:
    sample_rotation = _rotation_sample_quaternion(sample.rotation_deg)
    return TRS(
        translation_cm=tuple(
            _f32(float(authored.translation_cm[index]) + float(sample.offset_cm[index]))
            for index in range(3)
        ),
        rotation_quat=_quat_multiply(authored.rotation_quat, sample_rotation),
        scale=(
            _f32(float(authored.scale[0]) * float(sample.uniform_scale)),
            _f32(float(authored.scale[1]) * float(sample.uniform_scale)),
            _f32(
                float(authored.scale[2]) *
                float(sample.uniform_scale) *
                float(sample.vertical_scale)
            ),
        ),
    )


def resolve_composite(
    root: str,
    seed: int,
    composites: Mapping[str, Composite],
    profiles: Mapping[str, PlacementProfile],
    raw_hashes: Mapping[ResourceKey, str],
    resource_dependencies: Mapping[ResourceKey, Sequence[ResourceKey]] | None = None,
) -> ResolvedPlan:
    """Resolve one immutable plan after seed-free closure validation."""
    dependencies = resource_dependencies or {}
    closure = build_source_closure(
        root,
        composites,
        profiles,
        raw_hashes,
        dependencies,
    )
    decisions: list[SelectionDecision] = []
    draws: list[DrawTraceEntry] = []
    leaves: list[ResolvedLeaf] = []
    selected_dependencies: list[str] = []
    selected_seen: set[str] = set()

    def add_selected(value: str) -> None:
        if value not in selected_seen:
            selected_seen.add(value)
            selected_dependencies.append(value)

    def add_selected_resource(key: ResourceKey) -> None:
        add_selected(str(key))
        for dependency in sorted(tuple(dependencies.get(key, ())), key=str):
            add_selected_resource(dependency)

    def add_leaf(kind: str, resource: str, world: TRS, origin: str) -> None:
        leaves.append(ResolvedLeaf(kind, resource, world, origin))
        if kind == "mesh":
            add_selected_resource(ResourceKey("static_mesh", resource))
        elif kind == "actor":
            add_selected(f"actor:{resource}")

    def walk_composite(name: str, parent: TRS, prefix: str) -> None:
        composite = composites[name]
        for index, node in enumerate(composite.nodes):
            walk_node(node, parent, f"{prefix}:nodes[{index}]")

    def walk_selected_option(
        option: RandomOption,
        world: TRS,
        option_path: str,
    ) -> None:
        if option.kind == "empty":
            return
        resource = option.resource or ""
        if option.kind == "composite":
            add_selected_resource(ResourceKey("composite", resource))
            walk_composite(resource, world, f"{option_path}>{resource}")
        else:
            add_leaf(option.kind, resource, world, option_path)

    def walk_node(node: Node, parent: TRS, node_path: str) -> None:
        stream = (
            node_random_stream(seed, node_path)
            if node.kind == "random" or node.profile is not None
            else None
        )
        selected: SelectionDecision | None = None
        if node.kind == "random":
            assert stream is not None
            selected = select_weighted(stream, node_path, node.options)
            decisions.append(selected)
            draws.append(DrawTraceEntry(
                path=node_path,
                role="selection",
                raw_u32=selected.raw_u32,
                unit=selected.unit,
                sample=selected.target,
            ))

        local = node.transform
        if node.profile is not None:
            assert stream is not None
            profile = profiles[node.profile]
            add_selected_resource(ResourceKey("placement_profile", profile.name))
            local = _apply_profile(
                local,
                _sample_profile(stream, node_path, profile, draws),
            )
        world = compose_trs(parent, local)

        if node.kind == "mesh":
            add_leaf("mesh", node.resource or "", world, node_path)
        elif node.kind == "actor":
            add_leaf("actor", node.resource or "", world, node_path)
        elif node.kind == "composite":
            resource = node.resource or ""
            add_selected_resource(ResourceKey("composite", resource))
            walk_composite(resource, world, f"{node_path}>{resource}")
        elif node.kind == "random":
            assert selected is not None
            option = node.options[selected.option]
            option_path = f"{node_path}/options[{selected.option}]"
            walk_selected_option(option, world, option_path)

        for child_index, child in enumerate(node.children):
            walk_node(child, world, f"{node_path}/children[{child_index}]")

    walk_composite(root, IDENTITY_TRS, root)

    signature_document = {
        "v": 1,
        "resolver": RESOLVER_TAG,
        "seed": seed,
        "closure": closure.closure_hash,
        "decisions": [decision.signature_document() for decision in decisions],
        "leaves": [leaf.signature_document() for leaf in leaves],
    }
    preimage = canonical_json_bytes(signature_document)
    return ResolvedPlan(
        seed=seed,
        closure=closure,
        decisions=tuple(decisions),
        draws=tuple(draws),
        leaves=tuple(leaves),
        selected_dependencies=tuple(selected_dependencies),
        signature_preimage=preimage,
        resolved_signature=_blake3_160(preimage),
    )


__all__ = [
    "Composite",
    "DrawTraceEntry",
    "IDENTITY_TRS",
    "Node",
    "PlacementProfile",
    "RANDOM_STREAM_TAG",
    "RESOLVER_TAG",
    "RandomOption",
    "RandomReferenceError",
    "RandomStream",
    "Range",
    "ResolvedLeaf",
    "ResolvedPlan",
    "ResourceKey",
    "SelectionDecision",
    "SampledProfile",
    "SourceClosure",
    "TRS",
    "build_source_closure",
    "compose_trs",
    "node_random_stream",
    "path_hash64",
    "placement_state",
    "raw_payload_hash",
    "resolve_composite",
    "sample_placement_profile",
    "select_weighted",
]
