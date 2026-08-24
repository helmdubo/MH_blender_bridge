"""Small name-keyed in-memory DTOs shared by v4 host adapters."""

from dataclasses import dataclass, field

@dataclass(frozen=True)
class CompositeTransform:
    """One v4 placement in canonical UE units and axis convention."""

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


@dataclass
class Node:
    kind: str
    transform: CompositeTransform = IDENTITY_TRANSFORM
    name: str | None = None
    resource: str | None = None
    children: list = field(default_factory=list)


@dataclass
class Composite:
    name: str
    nodes: list = field(default_factory=list)


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
]
