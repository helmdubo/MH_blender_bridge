# G1 — FBX passport transport (Blender receipt)

> **SPIKE EVIDENCE ONLY.** Active contract is Clean Sources v2 in
> `docs/05_source_schema_v1.md`; Carrier B key is normative
> `mh.fbx_passport`. This receipt's provisional `mh_fbx_passport`, MESH-only
> wording and any Source Schema v1 statements are superseded. The measurements
> remain evidence; full carrier acceptance still requires the independent UE
> FBX SDK half plus all current Model-node/Combined-LOD/auxiliary cases.

Status of this historical receipt: **Blender half PASS; full G1 BLOCKED on the
independent UE FBX SDK read.** It is not a production codec.
Capability probes use `MH_W_FBX_CARRIER_READER_UNAVAILABLE`; production FBX
import remains fail-closed until the reader exists.

## Decision from this half-gate

Use provisional property key `mh_fbx_passport` and **Carrier B**: write the
same canonical one-line JSON string as a custom property on every exported
MESH Model node. On read, collect every MESH Model copy and require exact byte
equality before parsing it. Every MESH Model also carries an integer
`mh_lod_level`; the common passport enumerates the set once as `lod_levels`.
There is no per-file `lod_level` passport field.

Carrier A (one Empty named `MH_PASSPORT_SENTINEL`) survived both Blender
importers, including re-export, but is rejected as the primary carrier: it adds
a structural FBX node and makes discovery depend on a non-payload node surviving
the eventual UE static-mesh import path. It remains a comparison probe only.

Carrier C is rejected for v2: post-writing Blender's binary FBX requires a
second parser/writer, adds a second publication failure boundary, and produced
no transport advantage in this spike. The test intentionally uses exactly one
exporter.

The passport serializer in the spike sorts object keys, uses compact separators,
rejects NaN/Infinity, normalizes every string and key to Unicode NFC, emits raw
UTF-8, and forbids CR/LF in the resulting property. It is isolated under
`tools/spikes`; it is not imported by the add-on.

## Reproduction and stable artifacts

Run from repository root with Blender 4.5.12 LTS or later:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 4.5\blender.exe' `
  --background --factory-startup `
  --python tools\spikes\fbx_passport_transport.py -- `
  --outdir dist\spikes\g1-passport
```

`dist/` is deliberately ignored by Git. The stable, reproducible handoff paths
are:

- `dist/spikes/g1-passport/one_object.fbx`
- `dist/spikes/g1-passport/multi_shared_hierarchy_long.fbx`
- `dist/spikes/g1-passport/combined_lods.fbx`
- `dist/spikes/g1-passport/combined_lods.reexport.fbx`
- `dist/spikes/g1-passport/g1_results.json`

All `.fbx` files are binary (`Kaydara FBX Binary`). The JSON is the complete
machine-readable receipt and records every Model property's count/equality/hash,
plus hierarchy, transforms, normal error, shared-datablock observation, file
size and timing. It does not duplicate the 256 KiB raw property into the receipt.

Focused verification:

```powershell
& 'C:\Program Files\Blender Foundation\Blender 4.5\blender.exe' `
  --background --factory-startup --python-expr `
  "import pytest,sys; sys.exit(pytest.main(['tests/test_fbx_passport_spike_bpy.py','-q','-s']))"
```

Result on 2026-08-18: `2 passed`.

## Measured matrix

Environment: Blender `4.5.12 LTS`, Windows x64. Exporter:
`bpy.ops.export_scene.fbx` (`io_scene_fbx`), `use_custom_props=True`. Readers:

1. `bpy.ops.import_scene.fbx` — Python `io_scene_fbx`;
2. `bpy.ops.wm.fbx_import` — Blender native ufbx path.

Representative run (timings are diagnostic, not budgets):

| Case | MESH Models | Passport UTF-8 bytes | FBX bytes | Export ms | Python import ms | Native import ms |
|---|---:|---:|---:|---:|---:|---:|
| one object (cold exporter) | 1 | 455 | 13,468 | 492.73 | 32.66 | 1.22 |
| multi/shared/hierarchy/long | 3 | 262,617 | 1,065,436 | 7.25 | 6.41 | 3.89 |
| combined LOD0 + two LOD1 Models | 3 | 463 | 16,892 | 5.24 | 4.37 | 1.21 |
| combined LOD re-export | 3 | 463 | 16,892 | 4.46 | 4.41 source read | see JSON |

Observed:

- Carrier B arrived on every MESH Model through both readers; all copies were
  exactly equal to the authored Python string, including the 262,617-byte JSON.
- NFC text `Гараж_é` arrived exactly; an NFD input is canonicalized before
  export.
- Carrier A also arrived exactly through both readers. This proves it works in
  Blender, not that a UE StaticMesh importer retains it.
- Local pivots, parent-child hierarchy, and world translations round-tripped to
  six decimal places through both readers.
- Custom corner normals round-tripped with maximum component absolute error
  `1.6e-5`, below the spike threshold `5e-5`.
- Three Models backed by two mesh datablocks still imported as two datablocks
  through both readers.
- The amended LOD probe is one `combined_lods.fbx`: one LOD0 MESH Model and two
  LOD1 MESH Models. Every Model carries the same passport plus its own integer
  `mh_lod_level` (`0, 1, 1`). The passport contains `lod_levels:[0,1]` and does
  not contain a file-level `lod_level` field.
- Python-import then FBX re-export preserved all three Carrier B copies and all
  three integer `mh_lod_level` values through both readers. The separate rich
  case retains the 256 KiB Unicode/passport stress measurement. Equal semantic
  contents do not imply byte-identical FBX files; binary fingerprints may differ
  after re-export.

Blender 4.5 native ufbx has a separate shared-datablock accounting defect in
this probe: two Models reference `g1_shared_mesh`, but `Mesh.users` reports `1`;
Python `io_scene_fbx` reports `2`. Geometry, hierarchy, normals, and passports
are present and correct. The harness records
`mesh_datablock_user_counts_consistent=false` for that native case and retains
the affected objects until process exit to avoid Blender's unrelated ID
decrement diagnostic. This does not select a different passport carrier, but it
must not be hidden from host-adapter work.

## Copy-paste UE handoff (required to unblock G1)

Regenerate the artifacts with the command above, then give the UE executor this
checklist verbatim:

```text
G1 UE runtime check — read only; do not change Source Schema v1.

Inputs (repository-relative):
  dist/spikes/g1-passport/one_object.fbx
  dist/spikes/g1-passport/multi_shared_hierarchy_long.fbx
  dist/spikes/g1-passport/combined_lods.fbx
  dist/spikes/g1-passport/combined_lods.reexport.fbx
  dist/spikes/g1-passport/g1_results.json

Use the direct FBX SDK seam in FMHFbxBackend (not UFbxFactory asset metadata).
For each FBX:
1. Enumerate FBX Model nodes backed by Mesh geometry.
2. Read the KString custom property named "mh_fbx_passport" from every such
   Model; preserve its UTF-8 bytes before JSON parsing. Also read the integer
   Model property "mh_lod_level".
3. Require 1 passport copy in one_object and 3 copies in each multi-object file.
4. Require all MESH Model copies within one file to be byte-identical.
5. Parse JSON and require schema="mh.fbx_passport", schema_version=1,
   kind="static_mesh", and a valid resource_uid.
6. In multi_shared_hierarchy_long.fbx require property length 262,617 UTF-8
   bytes and exact NFC
   value properties.unicode_nfc_probe="Гараж_é".
7. In combined_lods.fbx require exactly three MESH Models:
     g1_lod0 -> mh_lod_level integer 0
     g1_lod1_part_a -> mh_lod_level integer 1
     g1_lod1_part_b -> mh_lod_level integer 1
   Require the identical passport on all three Models, passport lod_levels
   exactly [0,1], and absence of a top-level passport field named lod_level.
   The current canonical passport is 463 UTF-8 bytes with SHA-256
   83b68b94b18706337750f26dffb8df0094024d29d8731ee157487d50918f4e14.
   Repeat the same checks for combined_lods.reexport.fbx.
8. For fbxdump, record every Mesh-backed Model name and show both UserProperties
   on each: mh_fbx_passport as KString and mh_lod_level as integer. Expected
   copy/level sequence after sorting by Model name is 3 / [0,1,1]. Do not infer
   the level from Model names; names only make this fixture readable.
9. Separately report whether the Empty sentinel property is visible. Its result
   is comparison data only; Carrier B is the candidate.
10. Record UE version/build, FBX SDK entry point, per-file MESH Model count,
   per-copy byte length and SHA-256, parse result, and any importer diagnostics.

Return a machine-readable receipt plus log excerpt. Do not declare G1 green if
the direct SDK read was replaced by a post-import UStaticMesh metadata guess.
```

Until that receipt exists, UE runtime is explicitly **BLOCKED** and full G1 is
not accepted.
