"""Structural validation of golden/expected_diffs/*.json.

These files are the stage-B/C test specification (mh.diff_report §7.3),
so they must themselves obey the contract: known flags in the fixed
table order, CREATE/REMOVE exclusivity, and every UID resolvable in the
golden UID table.
"""

import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))

from golden_uids import UIDS  # noqa: E402

DIFFS_DIR = os.path.join(REPO_ROOT, "golden", "expected_diffs")

# Fixed flag order per docs/01_bundle_schema_v1.md §7.2.
FLAG_ORDER = [
    "CREATE", "REMOVE", "RENAME", "UPDATE_GEOMETRY", "UPDATE_TRANSFORM",
    "UPDATE_PROPERTIES", "REPARENT", "UPDATE_RESOURCE", "UPDATE_KIND",
    "EXTERNAL_UNRESOLVED",
]
RESOURCE_FLAGS = {"CREATE", "REMOVE", "RENAME", "UPDATE_GEOMETRY",
                  "UPDATE_PROPERTIES", "EXTERNAL_UNRESOLVED"}
NODE_FLAGS = {"CREATE", "REMOVE", "RENAME", "UPDATE_TRANSFORM",
              "UPDATE_PROPERTIES", "REPARENT", "UPDATE_RESOURCE", "UPDATE_KIND"}

EXPECTED_MUTATIONS = {
    "rename_object", "rename_collection", "linked_duplicate",
    "make_single_user", "delete_node", "edit_geometry", "reparent_node",
}

KNOWN_UIDS = set(UIDS.values())


def load_all():
    docs = {}
    for fname in os.listdir(DIFFS_DIR):
        if fname.endswith(".json"):
            with open(os.path.join(DIFFS_DIR, fname)) as f:
                docs[fname[:-5]] = json.load(f)
    return docs


def check_flags(ops, allowed, context):
    assert ops, f"{context}: empty op set must be omitted, not listed"
    assert len(set(ops)) == len(ops), f"{context}: duplicate flags"
    for op in ops:
        assert op in allowed, f"{context}: flag {op} not valid in this space"
    order = [FLAG_ORDER.index(op) for op in ops]
    assert order == sorted(order), f"{context}: flags not in table order"
    if "CREATE" in ops or "REMOVE" in ops:
        assert len(ops) == 1, f"{context}: CREATE/REMOVE must be exclusive"


def test_all_mutations_covered():
    assert set(load_all()) == EXPECTED_MUTATIONS


def test_report_schema_header():
    for name, doc in load_all().items():
        assert doc["schema"] == "mh.diff_report", name
        assert doc["schema_version"] == 1, name
        assert set(doc) == {"schema", "schema_version", "resources", "nodes"}, name


def test_flags_valid_and_ordered():
    for name, doc in load_all().items():
        for uid, ops in doc["resources"].items():
            check_flags(ops, RESOURCE_FLAGS, f"{name}/resources/{uid}")
        for comp_uid, nodes in doc["nodes"].items():
            assert nodes, f"{name}: composite {comp_uid} with no node ops"
            for node_uid, ops in nodes.items():
                check_flags(ops, NODE_FLAGS, f"{name}/nodes/{comp_uid}/{node_uid}")


def test_uids_resolve_in_golden_table():
    for name, doc in load_all().items():
        for uid in doc["resources"]:
            assert uid in KNOWN_UIDS, f"{name}: unknown resource uid {uid}"
        for comp_uid, nodes in doc["nodes"].items():
            assert comp_uid in KNOWN_UIDS, f"{name}: unknown composite uid {comp_uid}"
            for node_uid in nodes:
                assert node_uid in KNOWN_UIDS, f"{name}: unknown node uid {node_uid}"


def test_markdown_renders_in_sync():
    """The generated .md must exist and be current for every .json."""
    sys.path.insert(0, os.path.join(REPO_ROOT, "tools"))
    import render_expected_diffs as r
    for name, doc in load_all().items():
        md_path = os.path.join(DIFFS_DIR, f"{name}.md")
        assert os.path.exists(md_path), f"{name}.md missing — run render_expected_diffs.py"
        with open(md_path) as f:
            assert f.read() == r.render(name, doc), (
                f"{name}.md stale — re-run tools/render_expected_diffs.py")
