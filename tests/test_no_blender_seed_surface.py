"""V5-S3 forbids every Blender production seed surface."""

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parent.parent
BLENDER_ROOT = REPO_ROOT / "addon" / "mh4blend"
SEED_TOKEN = re.compile(r"instance_?seed|(^|[^a-z])seed([^a-z]|$)", re.I)


def test_blender_production_has_no_seed_or_instance_seed_surface():
    offenders = []
    for path in sorted(BLENDER_ROOT.rglob("*.py")):
        for line_number, line in enumerate(
                path.read_text("utf-8").splitlines(), 1):
            if SEED_TOKEN.search(line):
                offenders.append(
                    f"{path.relative_to(REPO_ROOT)}:{line_number}:{line.strip()}")
    assert offenders == []
