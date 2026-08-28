# V5-S6.1 Dagor bridge probe — 2026-08-28

Non-normative diagnostic evidence only. Repository, Git index, H: sources, installed Blender/addon, and Engine were not modified. All generated inputs, private addon copy/cache/preferences, venv, reports and `.blend` snapshot are under this directory.

## 1. Real dag4blend import and inheritance

The complete owner document 13 was read before the probe. Actual entry point: `bpy.ops.dt.cmp_import`, not MH's strict reader and not a replacement hierarchy parser.

- Blender: **4.5.12 LTS**, build `84afd5f785f7`; binary `C:/Program Files/Blender Foundation/Blender 4.5/blender.exe`.
- dag4blend: **2.12.0**, origin `C:/Users/helmd/AppData/Roaming/Blender Foundation/Blender/4.5/scripts/addons/dag4blend`.
- Loaded byte-identical private copy: `user/scripts/addons/dag4blend` beneath this probe directory. Installed/private `cmp/cmp_import.py` SHA256 before/after: `e007ccff6231eaf230d54df33bb600ddb977cf22e30bff6d3d26869e2bbc28ba`.
- Installed copy contains the existing random type/weight mirror overlay; compared with repository reference, these are the only `cmp_import.py` line changes. No importer code was patched for the probe.
- Startup: `--background --factory-startup --disable-autoexec --python-exit-code 19 --python probe_dag4blend.py`; BLENDER_USER_RESOURCES/CONFIG/SCRIPTS/DATAFILES/EXTENSIONS all pointed to private `user/`; bytecode writing disabled.
- `with_sub_cmp`, `with_dags`, `with_lods`, `refresh_cache` were false. A private cache supplied type/path hints for two existing real DAG files; those payloads were not imported. This isolates hierarchy/property transport, not full geometry/export acceptance.
- Only UI popup/show-text sinks were redirected in the harness. Operator, parser, collection creation, transforms and dagorprops assignment were unmodified.

Five real-operator calls completed with `FINISHED`; the process exited 0 and saved `reports/dag4blend_probe.blend`.

| Input | Observed datamodel |
|---|---|
| Real `normandy_vernacular_debris_wood.composit.blk`: root collision=no, aboveHt=2 | Two Empty objects, both parent=None, only type:t in dagorprops; no placement properties. Root Collection has only name/type, no root Empty. |
| Synthetic root collision=yes | Same absence of placement controls from both children and Collection. This rules out treating the first probe's false as an indistinguishable default. |
| Synthetic root place_type=3 | No place_type on either child or Collection. |
| Synthetic local parent node collision=yes | Parent Empty has dagorprops collision=True; child without a local control has no collision property; explicit child collision=no remains False. Parent links are intact. |
| Synthetic omitted ent name | Mixed random helper contains a real instance plus an Empty with instance_collection=None and weight=2. All-empty random helper also exists and contains instance_collection=None, weight=1. dag4blend itself does not prune this helper. |

**Finding:** dag4blend stores local controls but does not resolve their inheritance into children. Local node ancestry survives and can be inspected by MH. Document-root controls are a different case: they are discarded before any root Empty/carrier exists. They cannot be recovered from the resulting Empty hierarchy alone. A source-reading or carrier policy would require owner direction; this probe does not invent one.

The actual importer log confirms the root loss:

```text
real fixture: Something went wrong on reading this line: placeOnCollision:b=noaboveHt:r=2
root yes:     Something went wrong on reading this line: placeOnCollision:b=yes
root mode 3:  Something went wrong on reading this line: place_type:i=3
```

The scene importer UI remembers a filepath, and the Text log contains warnings, but neither is a per-definition typed carrier for root placement controls. They were not treated as source authority or parsed back into values.

Evidence: `reports/dag4blend_datamodel.json` (complete object/collection/scene ID properties and importer Text logs), separate per-input JSON, `probe_dag4blend.py`, and synthetic companions under `inputs/`. Real fixture SHA256 is `d2082dfb3c33754708a7eade0889ccb02dc67c40ad75665ce965d7ffa782acf2`.

Harness incident: the first invocation used addon_utils.enable(default_set=False), so dag4blend's registration could not find its preference entry. Corrected to default_set=True inside the private session; no addon/source change. Original failure retained as `reports/initial_registration_harness_failure.txt`; it is not an import result.

## 2. Empty-ent census

Scope: all **26,089** `*.composit.blk` files under `H:/_Gaijin_Entertainment/EnlistedCDK/ActiveMatterCDK.2024.11.11/EnlistedCDK/develop/assets`; exact mall subset `rendinst_1lod/mall_omsky/` is **291** files. Includes were not expanded. Raw manifest SHA256 exactly matches the previous inventory: `fca1b1f644b6f73ae2a83e89ee9f87321e9bb69850facbba02498b1ed6bb0d4f`.

The structural scanner excludes comments and unrelated string literals. It records quoted `name:t=""`, missing name, nonempty name, and malformed/nonliteral name separately. File counts overlap and must not be added across rows.

| Source-level category | Files | Occurrences | Mall files | Mall occurrences |
|---|---:|---:|---:|---:|
| Any ent block | 4372 | 40676 ent | 51 | 417 ent |
| Explicit quoted empty name | **443** | **1219 ent** | **35** | **128 ent** |
| Omitted name | **795** | **5322 ent** | **4** | **8 ent** |
| Explicit nonempty name | 4363 | 34134 ent | 51 | 281 ent |
| Only explicit-empty/explicit-nonempty mixed node | 440 | 1195 nodes | 35 | 124 nodes |
| All direct ent explicitly empty | 4 | 9 nodes | 1 | 1 node |
| Explicit-empty plus unknown/missing name | 2 | 2 nodes | 0 | 0 |

There is one separately reported `ent{name:t=;}` in one file. It is not silently classified as a quoted empty value. No duplicate-name ent was observed.

### Separate dag4blend-default interpretation

The actual `cmp_ent` model initializes name to the empty string. The fifth real-operator probe confirms that an omitted name produces an option Empty with no instance collection and preserves its weight. Consequently these **separately labelled, model-derived** rows combine explicit-empty and omitted-name; malformed/nonliteral remains excluded:

| Category | Files | Occurrences | Mall files | Mall occurrences |
|---|---:|---:|---:|---:|
| Explicit-or-omitted empty ent | **1223** | **6541 ent** | **39** | **136 ent** |
| Mixed empty/nonempty node | **1213** | **6341 nodes** | **39** | **132 nodes** |
| All-empty node | **14** | **185 nodes** | **1** | **1 node** |
| Any node containing empty | 1223 | 6526 nodes | 39 | 133 nodes |

This second table describes the route's empty-name default, not successful import or Dagor runtime evaluation of every corpus file.

Manual source checks:

- Omitted name: `gameproj/africa_east/entities/buildings/decor/m_east_windows/composits/barricades/m_east_window_1200x1900_barricade_random.composit.blk`, line 12, `$.node[0].ent[7]` contains only weight=0.4. SHA256 `e2e995f4ab4055c7dc9d957203c602378c9e87d1b806319c0ad812ebd3fc9dd5`.
- Mixed and all-explicit-empty in one mall file: `rendinst_1lod/mall_omsky/composites/kids_store/sovmod_toy_car_b_type_a_random_cmp.composit.blk`; all-empty node is `$.node[5]`, line 73. The full file was read; other nodes have real+empty pairs. See census examples for its hash.
- Unknown syntax: `gameproj/africa_forsaken/entities/buildings/african_city_houses/african_city_house_e_abandoned/composits/african_city_house_e_abandoned_roof_metal_a_cmp.composit.blk`, `$.node[1].ent[0]`, line 10 is literally `ent{name:t=;}`. SHA256 `d2d2756a4c98efa1c10ec22004e280e451d5d174f9bbe014ebf4b51f48d2fabb`.

`issues=[]` means zero observed structural brace/encoding/per-file-change issues; it does **not** mean the production parser accepts every file. The census does not validate the whole BLK grammar, resolve effective resource types, expand includes, import geometry, or evaluate topology. Before/after path inventories and per-file size/mtime checks passed; this is still not a global atomic snapshot.

## 3. Artifact receipt

- `reports/dag4blend_datamodel.json`: SHA256 `167355cb8b2bfd0fd3f50fa503dc3c06df218b68ef76bee6617f5879b0b3b897`.
- `reports/empty_ent_inventory.json`: SHA256 `6287bdc250ca978c65bcc9888a05f3c42e5afb52b5bdb7153f04057b8bd38176`.
- `probe_dag4blend.py`: SHA256 `378772e385938317d9aae89c13ccdaafaedfa8691761bf3f954707edffa0b39d`.
- `scan_empty_ent.py`: SHA256 `f45436db6dd05182f4d18463c3403df9589c12111d357fddf769327eebf4d7c6`.

The census script uses the previous read-only diagnostic lexer snapshot `E:/MimirComposite_DagorInventory_20260828_R2/inventory.py`; hash `510e4af12a81f07fc856e541408abd347c923107757095c1c2c34e1bf63b1aee`. A private Python 3.13.1 venv without third-party packages runs the census; Blender uses its own interpreter. No repository test suite or UE gate was run for this bounded diagnostic task.
