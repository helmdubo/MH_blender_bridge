"""Shared fail-closed publication policy for ``.placement`` resources.

The policy is deliberately host-free.  Callers first plan the complete
placement identity set, then stage every new payload, revalidate the complete
set immediately before the first authoritative replace, and finally publish.
Existing exact-canonical sources are reuse-only and are never rewritten.
"""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
from typing import Iterable

from .canonical import validate_resource_name
from .payload_publish_v2 import atomic_publish_bytes, canonical_payload_path
from .placements import parse_placement_profile, placement_json_bytes
from .validate import MHValidationError

__all__ = [
    "PlacementPublicationPlan",
    "PlacementPublicationRequest",
    "PlacementPublicationResult",
    "StagedPlacementPublication",
    "plan_placement_publications",
    "publish_placement_publications",
    "revalidate_placement_publications",
    "scan_placement_candidates",
    "stage_placement_publications",
]


@dataclass(frozen=True)
class PlacementPublicationRequest:
    """One canonical profile requested by an authoritative source."""

    name: str
    canonical_bytes: bytes
    provenance: Path


@dataclass(frozen=True)
class PlacementPublicationPlan:
    """Preflight decision for one globally unique placement identity."""

    request: PlacementPublicationRequest
    target: Path
    should_write: bool


@dataclass(frozen=True)
class StagedPlacementPublication:
    """Exact-canonical staged bytes, or ``None`` for a reuse-only plan."""

    plan: PlacementPublicationPlan
    staged_path: Path | None


@dataclass(frozen=True)
class PlacementPublicationResult:
    """One completed write or byte-identical reuse receipt."""

    name: str
    target: Path
    provenance: Path
    byte_count: int | None
    written: bool
    reused: bool


def _raise(code, subjects, message):
    raise MHValidationError(code, subjects, message)


def _resolved_directory(value, *, label: str) -> Path:
    path = Path(value).resolve(strict=False)
    if not path.is_dir():
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [path],
            f"{label} must be an existing directory")
    return path


def _inside(root: Path, path: Path) -> bool:
    try:
        return os.path.commonpath([
            os.path.normcase(str(root)), os.path.normcase(str(path)),
        ]) == os.path.normcase(str(root))
    except ValueError:
        return False


def scan_placement_candidates(root, name: str) -> list[Path]:
    """Return every exact physical candidate for one logical identity."""

    source_root = _resolved_directory(root, label="Project Source Root")
    try:
        validate_resource_name(name)
    except (TypeError, ValueError):
        _raise(
            "MH_E_NONCANONICAL_RESOURCE_NAME", [name],
            "placement identity must match [a-z0-9_]+ exactly")
    expected = f"{name}.placement"
    matches: dict[str, Path] = {}
    for candidate in source_root.rglob("*"):
        if not candidate.is_file() \
                or candidate.name.casefold() != expected.casefold():
            continue
        if candidate.name != expected:
            _raise(
                "MH_E_NONCANONICAL_RESOURCE_NAME", [candidate],
                f"placement filename must be exactly {expected!r}")
        physical = candidate.resolve(strict=True)
        if not _inside(source_root, physical):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE", [candidate, physical],
                "placement source resolves outside Project Source Root")
        matches[canonical_payload_path(physical)] = physical
    return sorted(
        matches.values(), key=lambda value: str(value).replace("\\", "/"))


def _validate_exact_canonical_payload(
        payload: bytes, *, name: str, subjects, context: str) -> None:
    if not isinstance(payload, bytes):
        raise TypeError("placement canonical_bytes must be bytes")
    try:
        decoded = parse_placement_profile(payload, name=name)
        canonical = placement_json_bytes(decoded)
    except ValueError as exc:
        _raise(
            getattr(exc, "code", None)
            or "MH_E_PLACEMENT_PROFILE_GRAMMAR",
            subjects, f"{context} is invalid: {exc}")
    if payload != canonical:
        _raise(
            "MH_E_PLACEMENT_PROFILE_GRAMMAR", subjects,
            f"{context} is not exact canonical bytes")


def _coalesce_requests(
        requests: Iterable[PlacementPublicationRequest]
) -> list[PlacementPublicationRequest]:
    result: list[PlacementPublicationRequest] = []
    by_name: dict[str, PlacementPublicationRequest] = {}
    for request in requests:
        if not isinstance(request, PlacementPublicationRequest):
            raise TypeError(
                "placement requests must be PlacementPublicationRequest")
        try:
            validate_resource_name(request.name)
        except (TypeError, ValueError):
            _raise(
                "MH_E_NONCANONICAL_RESOURCE_NAME",
                [request.name, request.provenance],
                "placement identity must match [a-z0-9_]+ exactly")
        _validate_exact_canonical_payload(
            request.canonical_bytes, name=request.name,
            subjects=[request.name, request.provenance],
            context="requested placement profile")
        previous = by_name.get(request.name)
        if previous is None:
            by_name[request.name] = request
            result.append(request)
            continue
        if previous.canonical_bytes != request.canonical_bytes:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [request.name, previous.provenance, request.provenance],
                "placement sources with the same identity have different "
                "canonical bytes; overwrite/winner selection is forbidden")
        # Identical identities are one publication decision.  Preserve the
        # first provenance deterministically for the receipt.
    return result


def plan_placement_publications(
        requests: Iterable[PlacementPublicationRequest], *,
        source_root, output_dir) -> tuple[PlacementPublicationPlan, ...]:
    """Preflight a complete identity set without staging or writing files."""

    root = _resolved_directory(source_root, label="Project Source Root")
    output = _resolved_directory(output_dir, label="placement output_dir")
    if not _inside(root, output):
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [output, root],
            "placement output_dir must be inside Project Source Root")

    plans: list[PlacementPublicationPlan] = []
    for request in _coalesce_requests(requests):
        matches = scan_placement_candidates(root, request.name)
        if len(matches) > 1:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [request.name, *matches, request.provenance],
                "multiple physical placement profiles share one logical name")
        if matches:
            existing = matches[0]
            raw = existing.read_bytes()
            _validate_exact_canonical_payload(
                raw, name=request.name,
                subjects=[existing, request.provenance],
                context="existing placement profile")
            if raw != request.canonical_bytes:
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [request.name, existing, request.provenance],
                    "existing placement profile and requested source differ; "
                    "overwrite/winner selection is forbidden")
            plans.append(PlacementPublicationPlan(request, existing, False))
            continue

        target = output / f"{request.name}.placement"
        if os.path.lexists(target):
            _raise(
                "MH_E_INVALID_RESOURCE_SOURCE",
                [target, request.provenance],
                "placement target exists but is not a regular candidate file")
        plans.append(PlacementPublicationPlan(request, target, True))
    return tuple(plans)


def stage_placement_publications(
        plans: Iterable[PlacementPublicationPlan], *, staging_dir, source_root
) -> tuple[StagedPlacementPublication, ...]:
    """Stage and canonical-read-back every new profile, touching no authority."""

    directory = _resolved_directory(staging_dir, label="placement staging_dir")
    root = _resolved_directory(source_root, label="Project Source Root")
    if _inside(root, directory):
        _raise(
            "MH_E_INVALID_RESOURCE_SOURCE", [directory, root],
            "placement staging_dir must be outside Project Source Root")
    staged: list[StagedPlacementPublication] = []
    created: list[Path] = []
    try:
        for plan in plans:
            if not isinstance(plan, PlacementPublicationPlan):
                raise TypeError(
                    "placement plans must be PlacementPublicationPlan")
            if not plan.should_write:
                staged.append(StagedPlacementPublication(plan, None))
                continue
            path = directory / f"{plan.request.name}.placement"
            if os.path.lexists(path):
                raise FileExistsError(
                    f"placement staging target already exists: {path}")
            with path.open("xb") as stream:
                stream.write(plan.request.canonical_bytes)
                stream.flush()
                os.fsync(stream.fileno())
            created.append(path)
            raw = path.read_bytes()
            if raw != plan.request.canonical_bytes:
                raise OSError(
                    f"staged placement read-back differs from payload: {path}")
            _validate_exact_canonical_payload(
                raw, name=plan.request.name,
                subjects=[plan.request.name, plan.request.provenance, path],
                context="staged placement profile")
            staged.append(StagedPlacementPublication(plan, path))
    except Exception:
        for path in reversed(created):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        raise
    return tuple(staged)


def revalidate_placement_publications(
        plans: Iterable[PlacementPublicationPlan], *, source_root) -> None:
    """Revalidate the whole identity set before the first replace."""

    root = _resolved_directory(source_root, label="Project Source Root")
    for plan in plans:
        request = plan.request
        matches = scan_placement_candidates(root, request.name)
        if plan.should_write:
            if matches or os.path.lexists(plan.target):
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [request.name, *matches, plan.target,
                     request.provenance],
                    "placement identity changed after preflight; refusing "
                    "race publication")
            continue
        if len(matches) != 1 or matches[0] != plan.target:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [request.name, plan.target, *matches, request.provenance],
                "reused placement identity changed after preflight")
        raw = plan.target.read_bytes()
        _validate_exact_canonical_payload(
            raw, name=request.name,
            subjects=[plan.target, request.provenance],
            context="reused placement profile")
        if raw != request.canonical_bytes:
            _raise(
                "MH_E_AMBIGUOUS_RESOURCE_NAME",
                [request.name, plan.target, request.provenance],
                "reused placement profile diverged after preflight")


def publish_placement_publications(
        staged: Iterable[StagedPlacementPublication], *, source_root
) -> tuple[PlacementPublicationResult, ...]:
    """Publish staged profiles after one whole-set final revalidation."""

    rows = tuple(staged)
    plans = tuple(row.plan for row in rows)
    root = _resolved_directory(source_root, label="Project Source Root")

    # Validate all staged bytes before final identity revalidation.  Neither
    # phase touches source authority.
    payloads: dict[str, bytes] = {}
    for row in rows:
        plan = row.plan
        if not plan.should_write:
            if row.staged_path is not None:
                raise ValueError("reuse-only placement plan must not be staged")
            continue
        if row.staged_path is None or not row.staged_path.is_file():
            raise ValueError(
                f"missing staged placement payload for {plan.request.name}")
        raw = row.staged_path.read_bytes()
        if raw != plan.request.canonical_bytes:
            _raise(
                "MH_E_PLACEMENT_PROFILE_GRAMMAR",
                [plan.request.name, plan.request.provenance,
                 row.staged_path],
                "staged placement profile changed after staging")
        _validate_exact_canonical_payload(
            raw, name=plan.request.name,
            subjects=[plan.request.name, plan.request.provenance,
                      row.staged_path],
            context="staged placement profile")
        payloads[plan.request.name] = raw

    revalidate_placement_publications(plans, source_root=root)

    results: list[PlacementPublicationResult] = []
    for row in rows:
        plan = row.plan
        request = plan.request
        if not plan.should_write:
            results.append(PlacementPublicationResult(
                request.name, plan.target, request.provenance,
                None, False, True))
            continue

        def guard(
                profile_name=request.name, target=plan.target,
                provenance=request.provenance):
            raced = scan_placement_candidates(root, profile_name)
            if raced or os.path.lexists(target):
                _raise(
                    "MH_E_AMBIGUOUS_RESOURCE_NAME",
                    [profile_name, *raced, target, provenance],
                    "placement identity changed after preflight; refusing "
                    "race overwrite")

        payload = payloads[request.name]

        def validate_read_back(
                read_back, profile_name=request.name,
                provenance=request.provenance):
            if read_back != payload:
                _raise(
                    "MH_E_PLACEMENT_PROFILE_GRAMMAR",
                    [profile_name, provenance],
                    "staged placement profile failed exact read-back")
            _validate_exact_canonical_payload(
                read_back, name=profile_name,
                subjects=[profile_name, provenance],
                context="staged placement profile")

        receipt = atomic_publish_bytes(
            plan.target, payload, source_root=root,
            read_back_validator=validate_read_back,
            pre_replace_guard=guard)
        results.append(PlacementPublicationResult(
            request.name, plan.target, request.provenance,
            receipt["bytes"], True, False))
    return tuple(results)
