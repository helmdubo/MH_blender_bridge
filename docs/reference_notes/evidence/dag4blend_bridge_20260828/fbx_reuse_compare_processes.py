"""Read diagnostic artifacts; report exact differences, never normalize FBX."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
left = json.loads((ROOT / "reports/process_one.json").read_text())
right = json.loads((ROOT / "reports/process_two.json").read_text())
left_tree = json.loads((ROOT / "process_one/a_tree.json").read_text())
right_tree = json.loads((ROOT / "process_two/a_tree.json").read_text())
left_bytes = Path(left["stages"][0]["path"]).read_bytes()
right_bytes = Path(right["stages"][0]["path"]).read_bytes()
diffs = [{"path": key, "left": left_tree.get(key), "right": right_tree.get(key)}
         for key in sorted(set(left_tree) | set(right_tree)) if left_tree.get(key) != right_tree.get(key)]
report = {
    "runs": [left["run"], right["run"]],
    "pids": [left["pid"], right["pid"]],
    "baseline_raw_equal": left_bytes == right_bytes,
    "baseline_sha256": [hashlib.sha256(left_bytes).hexdigest(), hashlib.sha256(right_bytes).hexdigest()],
    "classification_plan_equal": left["classification_plan"] == right["classification_plan"],
    "full_parsed_tree_equal": left_tree == right_tree,
    "parsed_tree_differences": diffs,
    "normalization_or_semantic_hash_performed": False,
}
(ROOT / "reports/cross_process.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8", newline="\n")
print(json.dumps({k:v for k,v in report.items() if k != "parsed_tree_differences"}, indent=2))
print("difference_paths=" + json.dumps([r["path"] for r in diffs]))
