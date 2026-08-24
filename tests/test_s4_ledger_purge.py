"""S4 permanently removes the deprecated pre-v4 Ledger surface."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
EDITOR_SOURCE = (
    REPO_ROOT
    / "ue"
    / "MimirComposite"
    / "Source"
    / "MimirCompositeEditor"
)
COMMANDLET_SOURCE = (
    EDITOR_SOURCE
    / "Private"
    / "Diagnostics"
    / "MHAnalyzeSourcesCommandlet.cpp"
)


def _production_sources():
    return sorted(
        path
        for suffix in ("*.h", "*.cpp")
        for path in EDITOR_SOURCE.rglob(suffix)
    )


def test_deprecated_ledger_surface_is_absent_from_production():
    production_sources = _production_sources()
    stale_paths = [
        path.relative_to(REPO_ROOT).as_posix()
        for path in production_sources
        if any("ledger" in part.casefold() for part in path.parts)
    ]
    stale_references = []
    for path in production_sources:
        if "ledger" in path.read_text(encoding="utf-8").casefold():
            stale_references.append(path.relative_to(REPO_ROOT).as_posix())

    assert stale_paths == []
    assert stale_references == []


def test_analyze_sources_has_no_deprecated_ledger_flags():
    source = COMMANDLET_SOURCE.read_text(encoding="utf-8").casefold()
    assert "-ledger" not in source
    assert "-writeledger" not in source
    assert "ledger=" not in source
    assert "writeledger=" not in source
