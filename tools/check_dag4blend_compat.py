"""Headless compatibility check against the vendored real dag4blend RNA.

Run with the supported Blender version:

    blender -b --factory-startup --python-exit-code 1 \
        -P tools/check_dag4blend_compat.py

The script registers dag4blend's actual ``dagormat`` PropertyGroups, stores a
representative material payload in them without relying on project-specific
shader configuration, and verifies mh4blend extraction.  It is deliberately
separate from the self-contained pytest adapter fixture.
"""

from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile

import bpy


REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "reference"))
sys.path.insert(0, str(REPO_ROOT / "addon"))

from dag4blend.dagormat import dagormat as dagormat_module  # noqa: E402
from mh4blend.scene.material_extract import (  # noqa: E402
    extract_collection_materials,
)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    dagormat_module.register()
    try:
        scene = bpy.data.scenes.new("GEOMETRY")
        collection = bpy.data.collections.new("dag4blend_material")
        scene.collection.children.link(collection)
        mesh = bpy.data.meshes.new("dag4blend_material")
        mesh.from_pydata(
            [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)],
            [], [(0, 1, 2)])
        obj = bpy.data.objects.new("dag4blend_material", mesh)
        obj["mh_uid"] = "10000000-0000-0000-0000-000000000001"
        collection.objects.link(obj)

        material = bpy.data.materials.new("m_metal_rust")
        # Direct ID-property assignment avoids invoking dag4blend's node-tree
        # builder, whose shader registry is project-specific and irrelevant to
        # this transport-contract check. These are the same RNA storage fields
        # used by the enabled addon in an authoring file.
        material.dagormat["shader_class"] = "rendinst_simple"
        material.dagormat["sides"] = 0
        material.dagormat.optional["micro_detail_layer"] = 6
        material.dagormat.optional["micro_detail_layer_uv_scale"] = 16.3710003

        texture_root = os.path.join(
            tempfile.gettempdir(), "mh4blend_dag4blend_texture_root")
        texture_path = os.path.join(
            texture_root, "manmade_common", "metal_rust_a_tex_d.tif")
        material.dagormat.textures["tex0"] = texture_path
        material.dagormat.textures["tex1"] = ""
        obj.data.materials.append(material)

        materials, slots = extract_collection_materials(collection)
        assert len(materials) == 1
        extracted = materials[0]
        assert extracted.shader_class == "rendinst_simple"
        assert extracted.params == {
            "micro_detail_layer": 6,
            "micro_detail_layer_uv_scale": 16.3710003,
            "sides": 0,
        }
        assert extracted.disk_payload()["params"] == {
            "micro_detail_layer": 6,
            "micro_detail_layer_uv_scale": 16.371,
            "sides": 0,
        }
        assert extracted.textures == {"tex0": texture_path}
        assert len(slots) == 1
        assert slots[0].slot_name == "m_metal_rust"
        assert slots[0].material_uid == extracted.uid
        print("DAG4BLEND COMPATIBILITY: PASSED")
    finally:
        dagormat_module.unregister()


if __name__ == "__main__":
    main()
