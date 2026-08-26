"""Resolution-independent Source Protocol v5 source-closure traversal.

This module deliberately knows only the immutable resource graph.  Host state,
filesystem admission, staging, publication, and random resolution belong to
their respective adapters.  In particular, every random option participates
in this traversal regardless of weight.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass

from .canonical import validate_resource_name
from .composites import (
    CompositeValueError,
    iter_profile_references,
    iter_resource_references,
)
from .model import Composite

__all__ = [
    "CompositeSourceClosure",
    "ResourceKey",
    "build_composite_source_closure",
]


_RESOURCE_KINDS = frozenset({
    "static_mesh",
    "material",
    "composite",
    "placement_profile",
    "texture",
})


@dataclass(frozen=True, order=True)
class ResourceKey:
    """One immutable Source Protocol ``Kind + LogicalName`` identity."""

    kind: str
    name: str

    def __post_init__(self) -> None:
        if self.kind not in _RESOURCE_KINDS:
            raise ValueError(f"unsupported Source Protocol resource kind {self.kind!r}")
        validate_resource_name(self.name)

    def __str__(self) -> str:
        return f"{self.kind}:{self.name}"


@dataclass(frozen=True)
class CompositeSourceClosure:
    """One deterministic all-options closure ready for host preflight.

    ``composites_postorder`` is dependency-first and therefore always ends in
    ``root``.  ``resources`` follows the protocol publish dependency classes:
    profiles, meshes, then dependency-first composites.  Members are deduped
    by ResourceKey while preserving their first significant encounter.
    """

    root: ResourceKey
    composites_postorder: tuple[ResourceKey, ...]
    placement_profiles: tuple[ResourceKey, ...]
    static_meshes: tuple[ResourceKey, ...]
    resources: tuple[ResourceKey, ...]


def _error(code: str, path: str, message: str) -> CompositeValueError:
    return CompositeValueError(code, path, message)


def build_composite_source_closure(
    root_name: str,
    resolver: Mapping[str, Composite | None]
    | Callable[[str], Composite | None],
) -> CompositeSourceClosure:
    """Build the all-options composite closure without resolving randomness.

    The resolver is called at most once per reachable composite identity.
    Missing resources and cycles fail closed before a result is returned.
    Actor references are intentionally not filesystem payloads and therefore
    do not appear in this graph layer.
    """

    validate_resource_name(root_name)
    resolve = resolver.get if isinstance(resolver, Mapping) else resolver
    if not callable(resolve):
        raise TypeError("resolver must be a Mapping or callable")

    root = ResourceKey("composite", root_name)
    active: list[str] = []
    complete: set[str] = set()
    composite_postorder: list[ResourceKey] = []
    profiles: dict[ResourceKey, None] = {}
    meshes: dict[ResourceKey, None] = {}

    def visit(name: str) -> None:
        if name in active:
            cycle = active[active.index(name):] + [name]
            raise _error(
                "MH_E_COMPOSITE_CYCLE",
                name,
                f"composite dependency cycle: {' -> '.join(cycle)}",
            )
        if name in complete:
            return

        composite = resolve(name)
        if composite is None:
            raise _error(
                "MH_E_UNRESOLVED_COMPOSITE_REFERENCE",
                name,
                "composite resource cannot be resolved",
            )
        if not isinstance(composite, Composite):
            raise TypeError("composite resolver must return Composite or None")

        active.append(name)

        # These orders are independently significant to their publish classes.
        # The codec iterators include nested children and every random option.
        for profile_name in iter_profile_references(composite):
            profiles.setdefault(ResourceKey("placement_profile", profile_name), None)
        for mesh_name in iter_resource_references(composite, kind="mesh"):
            meshes.setdefault(ResourceKey("static_mesh", mesh_name), None)
        for dependency_name in iter_resource_references(
            composite, kind="composite"
        ):
            visit(dependency_name)

        active.pop()
        complete.add(name)
        composite_postorder.append(ResourceKey("composite", name))

    visit(root_name)

    ordered_profiles = tuple(profiles)
    ordered_meshes = tuple(meshes)
    ordered_composites = tuple(composite_postorder)
    return CompositeSourceClosure(
        root=root,
        composites_postorder=ordered_composites,
        placement_profiles=ordered_profiles,
        static_meshes=ordered_meshes,
        resources=ordered_profiles + ordered_meshes + ordered_composites,
    )
