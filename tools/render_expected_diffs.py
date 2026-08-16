"""Render golden/expected_diffs/*.json into human-readable Markdown.

The JSON files are the primary artifact (docs/01_bundle_schema_v1.md §7.3):
stage-B/C tests compare them literally against exporter/Analyzer output.
The .md files are GENERATED — never edit them by hand, re-run this script.

Usage: python3 tools/render_expected_diffs.py
"""

import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)
from golden_uids import UIDS  # noqa: E402

REPO_ROOT = os.path.dirname(TOOLS_DIR)
DIFFS_DIR = os.path.join(REPO_ROOT, "golden", "expected_diffs")

_BY_UID = {uid: path for path, uid in UIDS.items()}


def label(uid: str) -> str:
    return _BY_UID.get(uid, "<unknown>")


def render(name: str, doc: dict) -> str:
    lines = [
        f"# expected diff: {name}",
        "",
        "> GENERATED from the sibling .json by tools/render_expected_diffs.py — do not edit.",
        "> Формат и семантика флагов: docs/01_bundle_schema_v1.md §7.",
        "",
    ]
    lines.append("## Resources")
    lines.append("")
    if doc["resources"]:
        lines += ["| resource_uid | golden path | ops |", "|---|---|---|"]
        for uid, ops in doc["resources"].items():
            lines.append(f"| `{uid}` | {label(uid)} | {', '.join(ops)} |")
    else:
        lines.append("_нет операций (все ресурсы UNCHANGED)_")
    lines.append("")
    lines.append("## Nodes")
    lines.append("")
    if doc["nodes"]:
        lines += ["| composite | node_uid | golden path | ops |", "|---|---|---|---|"]
        for comp_uid, nodes in doc["nodes"].items():
            for node_uid, ops in nodes.items():
                lines.append(
                    f"| {label(comp_uid)} | `{node_uid}` | {label(node_uid)} "
                    f"| {', '.join(ops)} |")
    else:
        lines.append("_нет операций (все узлы UNCHANGED)_")
    lines.append("")
    return "\n".join(lines)


def main():
    names = sorted(
        f[:-5] for f in os.listdir(DIFFS_DIR) if f.endswith(".json"))
    for name in names:
        with open(os.path.join(DIFFS_DIR, f"{name}.json")) as f:
            doc = json.load(f)
        out = os.path.join(DIFFS_DIR, f"{name}.md")
        with open(out, "w") as f:
            f.write(render(name, doc))
        print(f"rendered {out}")


if __name__ == "__main__":
    main()
