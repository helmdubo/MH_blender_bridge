#!/usr/bin/env python3
"""Consistency checks for the normative-docs set of MimirComposite.

Run from any working directory: the repo root is resolved as the parent
of the directory containing this script (tools/../).

Prints one ``VIOLATION: ...`` line per problem found, then either
``normative docs: OK`` (exit 0) or exits 1 if any violation was found.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
INDEX_PATH = REPO_ROOT / "docs" / "NORMATIVE_INDEX.md"
RECIPE_MODEL_PATH = REPO_ROOT / "docs" / "16_recipe_model.md"
EVIDENCE_PREFIX = "docs/reference_notes/evidence/"
CONTRACTS_PREFIX = "docs/contracts/"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def rel(path: Path) -> str:
    return path.relative_to(REPO_ROOT).as_posix()


def active_docs() -> list[Path]:
    """README.md, KICKOFF_PROMPT.md, and docs/**/*.md minus archive/receipts."""
    out = []
    for name in ("README.md", "KICKOFF_PROMPT.md"):
        p = REPO_ROOT / name
        if p.exists():
            out.append(p)
    for p in sorted((REPO_ROOT / "docs").rglob("*.md")):
        r = rel(p)
        if r.startswith("docs/archive/") or r.startswith("docs/receipts/"):
            continue
        out.append(p)
    return out


def normative_paths(index_text: str) -> list[str]:
    """Repo-relative paths listed in the '## Нормативные' section of the index."""
    m = re.search(r"## Нормативные\n(.*?)(\n## |\Z)", index_text, re.S)
    if not m:
        return []
    section = m.group(1)
    return sorted(set(re.findall(r"`([^`]+\.md)`", section)))


def check_index_coverage(index_text: str) -> list[str]:
    violations = []
    has_evidence_mention = EVIDENCE_PREFIX in index_text
    has_contracts_mention = CONTRACTS_PREFIX in index_text
    for p in active_docs():
        r = rel(p)
        # Directory-level coverage: evidence bundles and per-slice executor
        # contracts are covered when the index names their directory.
        if r.startswith(EVIDENCE_PREFIX) or r.startswith(CONTRACTS_PREFIX):
            covered = has_contracts_mention if r.startswith(CONTRACTS_PREFIX) else has_evidence_mention
            if not covered:
                violations.append(f"VIOLATION: {r} not covered by docs/NORMATIVE_INDEX.md")
            continue
        if r not in index_text:
            violations.append(f"VIOLATION: {r} not mentioned in docs/NORMATIVE_INDEX.md")
    return violations


def check_status_headers(norm_paths: list[str]) -> list[str]:
    violations = []
    for r in norm_paths:
        p = REPO_ROOT / r
        if not p.exists():
            violations.append(f"VIOLATION: {r} listed in NORMATIVE_INDEX.md does not exist")
            continue
        head_lines = read_text(p).splitlines()[:8]
        if not any("Status: NORMATIVE" in line for line in head_lines):
            violations.append(f"VIOLATION: {r} missing 'Status: NORMATIVE' in first 8 lines")
    return violations


def check_archive_headers() -> list[str]:
    violations = []
    expected = "> Status: HISTORY · Do not use for implementation · Superseded by docs/16_recipe_model.md"
    archive_dir = REPO_ROOT / "docs" / "archive"
    if not archive_dir.exists():
        return violations
    for p in sorted(archive_dir.rglob("*.md")):
        lines = read_text(p).splitlines()
        first = lines[0] if lines else ""
        if first != expected:
            violations.append(f"VIOLATION: {rel(p)} first line is not the HISTORY header")
    return violations


def check_no_links_to_archive(norm_paths: list[str]) -> list[str]:
    violations = []
    patterns = ("](archive/", "](docs/archive/", "](./archive/", "](../archive/")
    for r in norm_paths:
        if r == rel(INDEX_PATH):
            continue
        p = REPO_ROOT / r
        if not p.exists():
            continue
        for lineno, line in enumerate(read_text(p).splitlines(), start=1):
            if any(pat in line for pat in patterns):
                violations.append(f"VIOLATION: {r}:{lineno} normative link into archive/")
    return violations


def check_receipts_not_normative() -> list[str]:
    violations = []
    receipts_dir = REPO_ROOT / "docs" / "receipts"
    if not receipts_dir.exists():
        return violations
    for p in sorted(receipts_dir.rglob("*")):
        if not p.is_file():
            continue
        try:
            text = read_text(p)
        except (UnicodeDecodeError, OSError):
            continue
        if "Status: NORMATIVE" in text:
            violations.append(f"VIOLATION: {rel(p)} under docs/receipts/ contains Status: NORMATIVE")
    return violations


def extract_removed_entities() -> tuple[list[str], tuple[int, int] | None, list[str]]:
    """Returns (identifiers, (start_line, end_line) of block in the file, violations)."""
    if not RECIPE_MODEL_PATH.exists():
        return [], None, ["VIOLATION: docs/16_recipe_model.md not found for removed-entities gate"]
    lines = read_text(RECIPE_MODEL_PATH).splitlines()
    start = end = None
    for i, line in enumerate(lines):
        if line.strip() == "```removed-entities":
            start = i
            for j in range(i + 1, len(lines)):
                if lines[j].strip() == "```":
                    end = j
                    break
            break
    if start is None or end is None:
        return [], None, ["VIOLATION: docs/16_recipe_model.md missing removed-entities block"]
    identifiers = []
    for line in lines[start + 1:end]:
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        identifiers.append(s)
    return identifiers, (start, end), []


def check_removed_entities() -> list[str]:
    identifiers, block_range, violations = extract_removed_entities()
    if not identifiers:
        return violations
    # Lexical zero applies to normative prose only: KICKOFF_PROMPT.md is the
    # owner directive that names what to delete, and docs/reference_notes/
    # holds dated research and the external audit (evidence, not normative).
    for p in active_docs():
        r = rel(p)
        if r == "KICKOFF_PROMPT.md" or r.startswith("docs/reference_notes/") or r.startswith(CONTRACTS_PREFIX):
            # Executor contracts, like the kickoff, name the entities a slice deletes.
            continue
        lines = read_text(p).splitlines()
        is_recipe_model = p.resolve() == RECIPE_MODEL_PATH.resolve()
        for lineno, line in enumerate(lines, start=1):
            if is_recipe_model and block_range and block_range[0] <= lineno - 1 <= block_range[1]:
                continue
            for name in identifiers:
                if re.search(r"\b" + re.escape(name) + r"\b", line):
                    violations.append(f"VIOLATION: removed entity {name} in {rel(p)}:{lineno}")
    # Code gate (KICKOFF §7.4): a removed entity is lexically absent from the
    # plugin sources too, tests included. The identifiers come from the same
    # block, so a slice that deletes code and a slice that documents it agree.
    code_root = REPO_ROOT / "ue" / "MimirComposite" / "Source"
    if code_root.exists():
        for p in sorted(code_root.rglob("*")):
            if p.suffix.lower() not in {".h", ".cpp", ".inl", ".cs"}:
                continue
            for lineno, line in enumerate(read_text(p).splitlines(), start=1):
                for name in identifiers:
                    if re.search("(?<![A-Za-z0-9_])" + re.escape(name) + "(?![A-Za-z0-9_])", line):
                        violations.append(f"VIOLATION: removed entity {name} in code {rel(p)}:{lineno}")
    return violations


def main() -> int:
    if not INDEX_PATH.exists():
        print("VIOLATION: docs/NORMATIVE_INDEX.md not found")
        return 1
    index_text = read_text(INDEX_PATH)
    norm_paths = normative_paths(index_text)

    violations = []
    violations += check_index_coverage(index_text)
    violations += check_status_headers(norm_paths)
    violations += check_archive_headers()
    violations += check_no_links_to_archive(norm_paths)
    violations += check_receipts_not_normative()
    violations += check_removed_entities()

    for v in violations:
        print(v)
    if violations:
        return 1
    print("normative docs: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
