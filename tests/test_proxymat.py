"""Pure gates for read-only dag4blend proxymat parsing."""

from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.proxymat import parse_proxymat_text


def test_proxymat_mirrors_space_stripping_and_last_duplicate_wins():
    parsed = parse_proxymat_text('''
class : t = "rendinst_tree_colored"
twosided : b = yes
script : t = "is_pivoted = 0"
script:t="wind_strength=1.25"
script:t="is_pivoted=1"
''')

    assert parsed.material_class == "rendinst_tree_colored"
    assert parsed.twosided is True
    assert parsed.params == {"is_pivoted": 1, "wind_strength": 1.25}


def test_proxymat_separates_macro_texture_provenance_from_textures():
    parsed = parse_proxymat_text('''
tex0:t="D:\\foreign\\tree_leaf_d.tif"
tex7:t="$(ASSET_NAME)_pivot_pos"
tex8:t="$(ASSET_NAME)_pivot_dir"
''')

    assert parsed.textures == {"tex0": r"D:\foreign\tree_leaf_d.tif"}
    assert parsed.macro_textures == {
        "tex7": "$(ASSET_NAME)_pivot_pos",
        "tex8": "$(ASSET_NAME)_pivot_dir",
    }
