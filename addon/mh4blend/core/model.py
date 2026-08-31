"""Small name-keyed in-memory DTOs shared by Source Protocol host adapters."""

from dataclasses import dataclass, field

@dataclass(frozen=True)
class CompositeTransform:
    """One v5 parent-local transform in canonical UE units/axes."""

    translation_cm: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rotation_quat: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0)

    def disk_dict(self) -> dict:
        return {
            "translation_cm": list(self.translation_cm),
            "rotation_quat": list(self.rotation_quat),
            "scale": list(self.scale),
        }


IDENTITY_TRANSFORM = CompositeTransform()


@dataclass(frozen=True)
class RandomOption:
    kind: str
    weight: float
    resource: str | None = None


@dataclass
class Node:
    kind: str
    transform: CompositeTransform = IDENTITY_TRANSFORM
    name: str | None = None
    resource: str | None = None
    profile: str | None = None
    options: list = field(default_factory=list)
    children: list = field(default_factory=list)
    # Source provenance metadata. ``None`` means the source never stated a
    # place_type; it is not a synonym for the explicit value zero. Neither
    # field reaches the resolver, ResolvedSignature or random draws.
    place_type: int | None = None
    appearance_seed_boundary: bool = False
    # Inline placement-v1 body (owner decision 2026-08-31, revising
    # OPEN-V5-15): the node carries its randomization ranges directly, the
    # way Dagor authors inline p2, instead of referencing a derived external
    # `.placement` resource. Mutually exclusive with ``profile``.
    placement: "PlacementProfile | None" = None


@dataclass
class Composite:
    name: str
    nodes: list = field(default_factory=list)


@dataclass(frozen=True)
class PlacementRange:
    base: float
    deviation: float


@dataclass(frozen=True)
class PlacementProfile:
    name: str
    offset_cm: tuple[PlacementRange, PlacementRange, PlacementRange] | None = None
    rotation_deg: tuple[PlacementRange, PlacementRange, PlacementRange] | None = None
    uniform_scale: PlacementRange | None = None
    vertical_scale: PlacementRange | None = None


@dataclass(frozen=True)
class MaterialSlot:
    slot_name: str


@dataclass
class MeshResource:
    name: str
    material_slots: list = field(default_factory=list)


@dataclass
class MaterialResource:
    name: str
    material_class: str | None = None
    library: str | None = None
    twosided: bool | None = None
    textures: dict = field(default_factory=dict)
    params: dict = field(default_factory=dict)


__all__ = [
    "Composite",
    "CompositeTransform",
    "IDENTITY_TRANSFORM",
    "MaterialResource",
    "MaterialSlot",
    "MeshResource",
    "Node",
    "PlacementProfile",
    "PlacementRange",
    "RandomOption",
]
