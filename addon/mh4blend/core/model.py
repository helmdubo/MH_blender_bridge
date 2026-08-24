"""Small name-keyed in-memory DTOs shared by v4 host adapters."""

from dataclasses import dataclass, field

from .canonical import P_ROTATION_QUAT, P_SCALE, P_TRANSLATION_CM


def _dec(value: int, precision: int) -> float:
    return value / 10 ** precision


@dataclass(frozen=True)
class QuantizedTransform:
    translation: tuple
    rotation: tuple
    scale: tuple

    def disk_dict(self) -> dict:
        return {
            "translation_cm": [
                _dec(value, P_TRANSLATION_CM) for value in self.translation],
            "rotation_quat": [
                _dec(value, P_ROTATION_QUAT) for value in self.rotation],
            "scale": [_dec(value, P_SCALE) for value in self.scale],
        }


IDENTITY_TRANSFORM = QuantizedTransform(
    translation=(0, 0, 0),
    rotation=(0, 0, 0, 10 ** P_ROTATION_QUAT),
    scale=(10 ** P_SCALE,) * 3,
)


@dataclass
class Node:
    kind: str
    local_transform: QuantizedTransform = IDENTITY_TRANSFORM
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
    "IDENTITY_TRANSFORM",
    "MaterialResource",
    "MaterialSlot",
    "MeshResource",
    "Node",
    "QuantizedTransform",
]
