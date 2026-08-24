"""Name-keyed v4 DTO surfaces remain Blender-independent."""

from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.model import Composite, MaterialResource, Node


def test_v4_models_are_name_keyed_without_uid_fields():
    assert MaterialResource("wall").name == "wall"
    assert Composite("building", [Node("group", name="root")]).name == "building"
    assert not hasattr(Composite("building"), "uid")
