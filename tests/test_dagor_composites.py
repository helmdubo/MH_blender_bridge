"""Bpy-free gates for the authoritative Dagor composite reader."""

from dataclasses import FrozenInstanceError
from pathlib import Path

import pytest

from addon.mh4blend.core.dagor_composites import (
    DagorCompositeError,
    DagorNode,
    DagorOption,
    iter_resource_tokens,
    parse_dagor_composite,
    parse_dagor_placement_include,
    read_dagor_composite,
)
from addon.mh4blend.core.placements import (
    parse_placement_profile,
    placement_json_bytes,
)


REPO_ROOT = Path(__file__).resolve().parent.parent
GAZ_ROOT = REPO_ROOT / "reference" / "dagor_fixtures" / "gaz53"
IDENTITY_TM = (
    (1.0, 0.0, 0.0),
    (0.0, 1.0, 0.0),
    (0.0, 0.0, 1.0),
    (0.0, 0.0, 0.0),
)


def test_gaz_graph_frontier_order_and_implicit_random_weights():
    root = read_dagor_composite(GAZ_ROOT / "gaz53_b_random_cmp.composit.blk")
    assert root.name == "gaz53_b_random_cmp"
    assert len(root.nodes) == 1
    assert root.nodes[0].kind == "group"
    assert root.nodes[0].transform.columns == IDENTITY_TM
    assert [(token.kind, token.name) for token in iter_resource_tokens(root)] == [
        ("composite", "gaz53_b_body_cmp"),
        ("composite", "gaz53_body_bc_random_cmp"),
    ]

    body = read_dagor_composite(GAZ_ROOT / "gaz53_b_body_cmp.composit.blk")
    body_frontier = [(token.kind, token.name) for token in iter_resource_tokens(body)]
    assert len(body_frontier) == 17
    assert body_frontier[:3] == [
        ("composite", "gaz53_b_window_front_cmp"),
        ("mesh", "gaz53_b_body"),
        ("mesh", "gaz53_b_bumper"),
    ]
    assert body_frontier[-2:] == [
        ("mesh", "gaz53_b_wiper_left"),
        ("mesh", "gaz53_b_wiper_right"),
    ]
    hood = body.nodes[0].children[12]
    assert hood.resource.name == "gaz53_b_hood"
    assert hood.transform.columns[3] == (2.07504, 1.42497, 0.103072)

    random = read_dagor_composite(
        GAZ_ROOT / "gaz53_body_bc_random_cmp.composit.blk")
    random_node = random.nodes[0]
    assert random_node.kind == "random"
    assert [option.resource.name for option in random_node.options] == [
        "gaz53_bread_b_cmp",
        "gaz53_wooden_b_cmp",
        "gaz53_wooden_c_cmp",
    ]
    assert [option.weight for option in random_node.options] == [1.0, 1.0, 1.0]
    assert [option.provenance.line for option in random_node.options] == [5, 6, 7]


def test_nested_source_order_types_weights_comments_and_immutability():
    document = parse_dagor_composite(
        b'''\
className:t="CoMpOsIt"
/* ignored node{ ent{ name:t="bad:unknown"; } } */
node{
  ent{ name:t="crate:ReNdInSt"; weight:r=0; }
  node{
    // braces and fake p2 in comments: } scale:p2=1,2
    ent{ name:t="house:PrEfAb"; weight:r=2.5; }
    ent{ name:t="logic:GaMeObJ"; }
  }
  ent{ name:t="nested:COMPOSIT"; weight:r=1; }
}
''',
        source="synthetic.composit.blk",
        name="synthetic",
    )
    root = document.nodes[0]
    assert [type(member) for member in root.members] == [
        DagorOption, DagorNode, DagorOption]
    assert root.kind == "random"
    assert root.children[0].kind == "random"
    assert [(token.kind, token.name) for token in iter_resource_tokens(document)] == [
        ("mesh", "crate"),
        ("mesh", "house"),
        ("actor", "logic"),
        ("composite", "nested"),
    ]
    assert [token.dagor_type for token in iter_resource_tokens(document)] == [
        "rendinst", "prefab", "gameobj", "composit"]
    assert root.provenance.path == "$.nodes[0]"
    assert root.children[0].options[1].provenance.path == (
        "$.nodes[0].children[0].options[1]")
    with pytest.raises(FrozenInstanceError):
        document.name = "changed"


@pytest.mark.parametrize("payload, message", [
    ('className:t="composit" node{ ent{ name:t="missing"; } }',
     "explicit name:type"),
    ('className:t="composit" node{ name:t="thing:unknown"; }',
     "unsupported Dagor resource type"),
    ('className:t="composit" node{ mystery:i=1; }',
     "unsupported node construct"),
    ('className:t="composit" node{ ent{ name:t="a:composit"; weight:r=-1; } }',
     "weight must be non-negative"),
    ('className:t="composit" node{ ent{ name:t="a:composit"; weight:r=0; } }',
     "random option total must be positive"),
    ('className:t="composit" node{ name:t="a:composit"; '
     'ent{ name:t="b:composit"; } }',
     "resource node cannot also contain random options"),
    ('className:t="composit" /* never closes', "unterminated block comment"),
])
def test_malformed_or_lossy_grammar_fails_closed(payload, message):
    with pytest.raises(DagorCompositeError, match=message) as caught:
        parse_dagor_composite(payload, source="broken.composit.blk")
    assert caught.value.code == "MH_E_COMPOSITE_GRAMMAR"
    assert caught.value.provenance.source == "broken.composit.blk"
    assert caught.value.provenance.line >= 1
    assert caught.value.provenance.column >= 1


def test_resource_node_preserves_ordinary_children():
    document = parse_dagor_composite(
        'className:t="composit" node{ name:t="parent:composit"; '
        'node{ name:t="child:rendinst"; } }',
        source="resource_children.composit.blk",
        name="resource_children",
    )
    parent = document.nodes[0]
    assert parent.kind == "composite"
    assert parent.resource.name == "parent"
    assert [(child.kind, child.resource.name) for child in parent.children] == [
        ("mesh", "child")]


def test_node_include_is_retained_but_other_carriers_and_inline_p2_fail_closed():
    document = parse_dagor_composite(
        'className:t="composit"\nnode{ include "profiles/random_car.blk"; }',
        source="profile.composit.blk",
        name="profile",
    )
    include = document.nodes[0].include
    assert include.path == "profiles/random_car.blk"
    assert include.provenance.path == "$.nodes[0].profile"
    assert include.provenance.line == 2

    constructs = (
        'include "random_profile.blk"',
        'offset_x:p2=10, 1',
        'node{ offset_x:p2=10, 1 }',
        'node{ ent{ include "random_profile.blk"; name:t="a:composit"; } }',
    )
    for construct in constructs:
        payload = f'className:t="composit"\n{construct}\n'
        with pytest.raises(DagorCompositeError, match="lossless conversion") as caught:
            parse_dagor_composite(payload, source="profile.composit.blk")
        assert caught.value.code == "MH_E_COMPOSITE_GRAMMAR"
        assert "profile.composit.blk:2:" in str(caught.value)


def test_admitted_include_p2_subset_maps_to_canonical_placement_profile():
    profile = parse_dagor_placement_include(
        '''\
// Source order is irrelevant to the typed profile.
rot_z:p2=[15,30]
offset_x:p2=10,1
offset_y:p2=-2, 0.5
offset_z:p2=0,0
rot_x:p2=5,10
rot_y:p2=-5,20
scale:p2=1,0.25
yScale:p2=[1,0.1]
''',
        source="scatter_profile.blk",
        name="scatter_profile",
    )
    canonical = placement_json_bytes(profile)
    assert parse_placement_profile(canonical, name="scatter_profile") == profile
    assert [[item.base, item.deviation] for item in profile.offset_cm] == [
        [10.0, 1.0], [-2.0, 0.5], [0.0, 0.0]]
    assert [[item.base, item.deviation] for item in profile.rotation_deg] == [
        [5.0, 10.0], [-5.0, 20.0], [15.0, 30.0]]
    assert (profile.uniform_scale.base,
            profile.uniform_scale.deviation) == (1.0, 0.25)
    assert (profile.vertical_scale.base,
            profile.vertical_scale.deviation) == pytest.approx((1.0, 0.1))


@pytest.mark.parametrize("payload, match", [
    ("offset_x:p2=1,0", "complete offset_x/offset_y/offset_z"),
    ("mystery:p2=1,0", "unsupported Dagor placement parameter"),
    ("scale:p2=1,0; scale:p2=1,0", "duplicate Dagor placement parameter"),
    ("scale:p2=0.5,0.5", "base - deviation"),
    ("offset_x:p2=0,-1; offset_y:p2=0,0; offset_z:p2=0,0",
     "deviation"),
])
def test_admitted_include_rejects_lossy_or_invalid_parameters(payload, match):
    with pytest.raises(DagorCompositeError, match=match):
        parse_dagor_placement_include(
            payload, source="bad_profile.blk", name="bad_profile")


def test_include_profile_name_is_exact_and_never_normalized():
    with pytest.raises(
            DagorCompositeError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        parse_dagor_placement_include(
            "scale:p2=1,0", source="Bad Profile.blk", name="Bad Profile")


def test_matrix_shape_duplicates_non_finite_and_wrong_class_are_rejected():
    cases = (
        'className:t="composit" node{ tm:m=[[1,0,0] [0,1,0] [0,0,1]] }',
        'className:t="composit" node{ tm:m=[[1,0,0] [0,1,0] [0,0,1] [1e999,0,0]] }',
        'className:t="composit" node{ name:t="a:composit"; name:t="b:composit"; }',
        'className:t="rendInst" node{}',
    )
    for payload in cases:
        with pytest.raises(DagorCompositeError) as caught:
            parse_dagor_composite(payload, source="matrix.composit.blk")
        assert caught.value.code == "MH_E_COMPOSITE_GRAMMAR"


def test_file_reader_rejects_wrong_suffix_and_invalid_utf8(tmp_path):
    wrong_suffix = tmp_path / "thing.blk"
    wrong_suffix.write_text('className:t="composit"', encoding="utf-8")
    with pytest.raises(DagorCompositeError) as suffix_error:
        read_dagor_composite(wrong_suffix)
    assert suffix_error.value.code == "MH_E_INVALID_RESOURCE_SOURCE"

    invalid_utf8 = tmp_path / "thing.composit.blk"
    invalid_utf8.write_bytes(b"\xff")
    with pytest.raises(DagorCompositeError) as encoding_error:
        read_dagor_composite(invalid_utf8)
    assert encoding_error.value.code == "MH_E_INVALID_RESOURCE_SOURCE"
