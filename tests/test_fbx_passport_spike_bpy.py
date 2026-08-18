"""Blender-side acceptance checks for the isolated ADR-v2 G1 spike."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import pytest


bpy = pytest.importorskip("bpy")


def _load_spike_module():
    path = (Path(__file__).resolve().parents[1] / "tools" / "spikes" /
            "fbx_passport_transport.py")
    spec = importlib.util.spec_from_file_location("mh_g1_fbx_passport", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_canonical_passport_is_stable_one_line():
    spike = _load_spike_module()
    first = spike.passport_document(name="probe")
    second = dict(reversed(list(first.items())))
    assert spike.canonical_passport(first) == spike.canonical_passport(second)
    encoded = spike.canonical_passport(first)
    assert "\n" not in encoded
    assert "\r" not in encoded
    assert encoded.startswith('{"exporter":')
    assert "Гараж_é" in encoded
    nfd = spike.passport_document(name="probe")
    nfd["properties"]["unicode_nfc_probe"] = "Гараж_e\u0301"
    assert "Гараж_é" in spike.canonical_passport(nfd)
    combined = json.loads(spike.canonical_passport(
        spike.passport_document(name="lod_probe", lod_levels=[0, 1])))
    assert combined["lod_levels"] == [0, 1]
    assert "lod_level" not in combined


def test_carrier_b_survives_both_blender_importers_and_reexport(tmp_path):
    spike = _load_spike_module()
    receipt = spike.run_spike(tmp_path)

    assert receipt["exporter"] == "bpy.ops.export_scene.fbx (io_scene_fbx)"
    assert receipt["importers"] == ["python_io_scene_fbx", "native_ufbx"]
    assert receipt["ue_runtime"].startswith("BLOCKED")
    assert receipt["carrier_c"].startswith("rejected")

    for case in receipt["cases"].values():
        assert case["passport_one_line"]
        assert case["unicode_nfc_utf8_present"]
        assert case["passport_is_unicode_nfc"]
        assert (tmp_path / case["fbx"]).read_bytes().startswith(
            b"Kaydara FBX Binary")
        for imported in case["imports"].values():
            assert imported["mesh_model_passports_equal"]
            assert imported["all_carrier_b_copies_byte_equal"]
            assert imported["carrier_b_copy_count"] == imported["mesh_count"]
            assert imported["mesh_lod_levels_are_int"]
            assert imported["carrier_a_survived"]
            assert imported["carrier_a_copy_count"] == 1
            assert set(imported["carrier_b_copy_sha256"]) == {
                case["passport_sha256"]}
            assert imported["carrier_a_copy_sha256"] == [
                case["passport_sha256"]]
            assert imported["local_locations_match_source"]
            assert imported["world_translations_match_source"]
            assert imported["corner_normals_within_5e_5"]
            assert imported["corner_normal_max_abs_error"] <= 5e-5

    long_case = receipt["cases"]["multi_shared_hierarchy_long"]
    assert long_case["passport_utf8_bytes"] >= 256 * 1024
    assert long_case["imports"]["python_io_scene_fbx"]["mesh_count"] == 3
    assert long_case["imports"]["native_ufbx"]["mesh_count"] == 3
    assert long_case["imports"]["python_io_scene_fbx"]["parents"] == {
        "g1_child": "g1_parent",
        "g1_independent": None,
        "g1_parent": None,
    }
    assert long_case["imports"]["native_ufbx"]["parents"] == {
        "g1_child": "g1_parent",
        "g1_independent": None,
        "g1_parent": None,
    }

    combined = receipt["cases"]["combined_lods"]
    assert combined["passport_summary"]["lod_levels"] == [0, 1]
    assert "lod_level" not in combined["passport_summary"]
    for imported in combined["imports"].values():
        assert imported["mesh_count"] == 3
        assert imported["carrier_b_copy_count"] == 3
        assert imported["mesh_lod_levels"] == {
            "g1_lod0": 0,
            "g1_lod1_part_a": 1,
            "g1_lod1_part_b": 1,
        }

    for imported in receipt["reexport"]["imports"].values():
        assert imported["mesh_model_passports_equal"]
        assert imported["carrier_b_copy_count"] == imported["mesh_count"] == 3
        assert imported["mesh_lod_levels"] == {
            "g1_lod0": 0,
            "g1_lod1_part_a": 1,
            "g1_lod1_part_b": 1,
        }
        assert imported["carrier_a_survived"]
        assert imported["local_locations_match_source"]
        assert imported["world_translations_match_source"]
        assert imported["corner_normals_within_5e_5"]
