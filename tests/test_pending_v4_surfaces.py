"""S1 keeps public material/composite surfaces fail-closed until S2/S3."""

from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.model import Composite, MaterialResource, Node
from mh4blend.scene.export_composite import export_composite_collection
from mh4blend.scene.import_composite import import_composite_file


def test_transitional_models_are_name_keyed():
    assert MaterialResource("wall").name == "wall"
    assert Composite("building", [Node("group", name="root")]).name == "building"


@pytest.mark.parametrize("operation", [
    export_composite_collection,
    import_composite_file,
])
def test_composite_surfaces_are_explicitly_fail_closed_until_s3(operation):
    with pytest.raises(RuntimeError, match="until slice S3"):
        operation(object())
