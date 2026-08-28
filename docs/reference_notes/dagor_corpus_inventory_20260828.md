# Dagor corpus inventory R2 — 2026-08-28

Read-only evidence receipt; not a protocol decision, production parser, or runtime parity claim. No source files were copied into the repository. No repository, Git index, H: source, Blender, or UE mutation was performed by this inventory task.

## Scope and reproducibility

Corpus root: `H:/_Gaijin_Entertainment/EnlistedCDK/ActiveMatterCDK.2024.11.11/EnlistedCDK/develop/assets`.
Scope: every `*.composit.blk` recursively under that root, discovered by `rg --files --hidden --no-ignore -g *.composit.blk`; no includes expanded. Mall subset is exactly `rendinst_1lod/mall_omsky/`, not similarly named copies under gameproj.

- 26,089 files; 241,577,444 raw bytes; mall subset 291 files.
- Before/after path inventory matched; each file's size/mtime checked around its read; no encoding, brace, or stability issues reported.
- Two full captures produced the identical manifest SHA256: `fca1b1f644b6f73ae2a83e89ee9f87321e9bb69850facbba02498b1ed6bb0d4f`.
- `inventory.py` SHA256: `510e4af12a81f07fc856e541408abd347c923107757095c1c2c34e1bf63b1aee`.
- `candidate_audit.py` SHA256: `4cbf03588c2bac21a9df6a5026614638e2f3e177289204e1d2e02fb69308d29d`.
- Anchor `rendinst_1lod/mall_omsky/sovmod_mall_omsky_a_cmp.composit.blk`: 5,657 bytes; SHA256 `f7746aeecf5a6678a908db6dfb6bc1f54d36e3a6e6ae53a368bb141573832ab1`.

`manifest.jsonl` contains relative path, byte length, and raw SHA256 for every file. SHA256 is an evidence digest here, not a Source Protocol hash. The capture is not an atomic filesystem snapshot.

## Declaration counts

All numbers below are unique files, not occurrences. A file may contain multiple values and therefore appear in several rows. Comments and unrelated string literals are excluded; type suffixes are inspected only in real quoted `name:t`, and explicit resource types only in real quoted `type:t`. Type comparison is case-insensitive for this inventory; original spellings are retained, not normalized into a proposed importer rule.

| Construct | All assets | Mall |
|---|---:|---:|
| place_type | 1710 | 7 |
| aboveHt | 14 | 0 |
| placeOnCollision | 52 | 0 |
| useCollisionNormal | 8 | 0 |
| ignoreParentInstSeed | 190 | 0 |
| quantizeTm | 3 | 0 |
| include directive | 257 | 1 |
| Any requested inline p2 | 453 | 6 |
| rot_x / rot_y / rot_z p2 | 56 / 365 / 76 | 0 / 6 / 0 |
| offset_x / offset_y / offset_z p2 | 171 / 61 / 169 | 3 / 0 / 3 |
| scale / yScale p2 | 52 / 20 | 0 / 0 |
| spline block | 492 | 0 |
| polygon block | 176 | 0 |
| gameObj explicit type or name suffix | 3674 | 35 |
| prefab explicit type or name suffix | 807 | 0 |
| physObj explicit type or name suffix | 6 | 0 |
| dynModel explicit type or name suffix | 1 | 0 |

Zero real declarations in both scopes: `place_on_collision`, `use_collision_norm`, `useParentInstSeed`, `colors`, `require`, `label`; resource types `fx`, `animChar`, `landClass`. `colors` and `require` named blocks were also checked. This does not claim absence inside excluded include payloads or outside the selected extension/root.

| Field value | All files | Mall files | All occurrences |
|---|---:|---:|---:|
| place_type:i=0 | 104 | 3 | 2528 |
| place_type:i=1 | 1213 | 3 | 13177 |
| place_type:i=2 | 111 | 1 | 1337 |
| place_type:i=3 | 641 | 0 | 9678 |
| place_type:i=4 | 303 | 0 | 3783 |
| place_type:i=6 | 78 | 0 | 465 |
| aboveHt:r=0 / 2 | 1 / 13 | 0 / 0 | 4 / 13 |
| placeOnCollision:b=no / yes | 14 / 46 | 0 / 0 | 14 / 188 |
| useCollisionNormal:b=yes | 8 | 0 | 8 |
| ignoreParentInstSeed:b=yes | 190 | 0 | 660 |
| quantizeTm:b=yes | 3 | 0 | 3 |

No place_type=5, other numeric values, opposite seed-flag values, or useParentInstSeed declarations were observed. Explicit zero is real source syntax, not missing data. Eight files have both legacy placement controls in one block; ten files have root and child placement controls. Their runtime priority/inheritance is not proven by their coexistence.

## gameObj inventory and Marker boundary

`gameobj_occurrences.jsonl` contains all 11,880 occurrences with logical name, raw name, exact spelling evidence, file hash/path, source-order nodepath and line. Evidence: 8,855 explicit suffixes + 3,025 same-block type declarations; no parent-random-type-only candidates occurred. `gameobj_names.json` groups 386 names. This is declaration inventory, not asset availability or resolved inherited type.

`mall_gameobj_names.json` contains the exact 22 mall names, paths and nodepaths: 466 occurrences in 35 files, all explicit suffixes. Names are:

`civil_loot_box`, `dummy_pivot`, `envi_probe_box`, `loot_box`, `loot_box_shop`, `loot_box_special_a`, `loot_box_vehicle_parts`, `loot_crate`, `loot_crate_rich`, `loot_indoor_floor`, `loot_indoor_medical`, `loot_indoor_military_floor`, `loot_indoor_military_shelves`, `loot_indoor_shelves`, `loot_indoor_shop`, `loot_indoor_table`, `loot_medic_box_wall_old`, `loot_military_crate_big_180x80`, `loot_military_crate_long_weapon_160x40`, `loot_military_crate_medium_120x60`, `loot_military_crate_small_50x40`, `loot_military_crate_small_ammo_30x20`.

Every record has `marker_role=unassigned`. `dummy_pivot` (19 mall occurrences) is only a candidate for an owner-reviewed Marker policy. Neither its name nor its identity transform establishes that role. No automatic gameObj-to-Marker mapping is proposed.

## Fixture candidates and manual checks

Paths below are relative to the corpus root. They remain raw external candidates, not accepted fixtures. Nodepath indices are zero-based source order per named block; `$` is the document root.

1. **Root controls, missing child controls, nonzero aboveHt — manually read.** `gameproj/normandy_invasion/entities/buildings/normandy_vernacular_buildings_large/composit_parts/normandy_vernacular_debris_wood.composit.blk`; SHA256 `d2082dfb3c33754708a7eade0889ccb02dc67c40ad75665ce965d7ffa782acf2`. Root has `placeOnCollision:b=no` line 2 and `aboveHt:r=2` line 3; both direct nodes omit local placement controls. A commented `rot_y:p2` was correctly excluded. This is source structure suitable for a root-inheritance test, not measured inheritance behavior.

2. **Root/child override and same-block legacy pair — manually read.** `gameproj/normandy_invasion/entities/buildings/fences/normandy_fence_c/normandy_fence_2000_flat_c_breach_a_debris_a.composit.blk`; SHA256 `de470f058f6f12a1993114863a9454bc65060ed4ed9d97799a1a018ac7f37ade`. Root collision=no; `$.node[1]` has collision=yes line 12 and collision-normal=yes line 13. This supplies a priority candidate but does not establish which runtime rule wins.

3. **Explicit zero — manually read.** `rendinst_1lod/mall_omsky/mall_omsky_office_building_a/composites/mall_omsky_office_building_a_window_d_cmp.composit.blk`; SHA256 `ea5cbfeade64db76a652d6e1c46a04edda59947f9511f903f26fef4fd84a4bcf`. `$.node[0].node[1].node[0]` line 28 and its sibling node[1] line 34 declare `place_type:i=0`. The same file also contains an ent-based random node with untyped nonempty name tokens.

4. **Marker candidate only — manually read.** `rendinst_1lod/mall_omsky/composites/canteen_tables/sovmod_mall_omsky_canteen_table_set_a_cmp.composit.blk`; SHA256 `5dfec91eaa9d7371d47f5077eed5ce17d2044a84dab12f7214aabf358e6883fc`. `$.node[0].node[0]` line 7 explicitly names `dummy_pivot:gameObj` with identity transform. The file does not declare a Marker role.

5. **quantizeTm — manually read.** `gameproj/africa_forsaken/entities/vehicle/nasams_launcher/afr_airport_nasams_launcher_cmp.composit.blk`; SHA256 `f43b53a674ebc20629b21b9a0e6408349b9ed150774dc6894d78c569da4f0aec`. Root line 2 declares quantizeTm=yes; one child is untyped and another explicitly has type=prefab. No quantization behavior was executed.

6. **Tiny structural import candidate.** `rendinst_1lod/mall_omsky/composites/doors/sovmod_mall_omsky_door_100x220_a_cmp.composit.blk`; 212 bytes; SHA256 `c2a29d544f986fa509d32ae91e71b3d4bc1bdbb2f3a9a1b630c6800b7299db1d`.

7. **Explicit nonzero place_type — manually read in full.** `rendinst_1lod/mall_omsky/composites/sovmod_mall_omsky_bunker_loot_cmp.composit.blk`; 4,737 bytes; SHA256 `125ab87fc8876a85ca50f6e4dbf048ed5107b0d68f23a9efed44f76443ae51f7`. Nested gameObj nodes explicitly declare place_type=1, e.g. `$.node[0].node[0]` line 9 and `$.node[0].node[1]` line 15. Selected from the existing bounded candidate catalogue; not claimed to be the globally smallest corpus example. Marker roles and placement behavior remain unassigned.

8. **ignoreParentInstSeed — manually read in full.** `gameproj/africa_forsaken/entities/indoor_stuff/clothes/composits/default/clothes_pile_a_cmp.composit.blk`; 164 bytes; SHA256 `b02e852346fb447d904b7215f888c298e7e905a4485cb2d51b07147526e4d2b9`. `$.node[0]` is a rendInst node declaring ignoreParentInstSeed=yes at line 7. No child sources/includes or expected runtime behavior were added.

**Missing natural fixture coverage:** no `require` declaration. No node-with-local-placement-control directly containing a node without local placement controls was found, even after scanning all nesting depths in all 1,767 lexical candidate files. Therefore the corpus does not provide the requested direct nontransitive node-inheritance fixture. The initial broad candidate selector incorrectly included `node -> ent`; this was corrected and such candidates were removed. A separate approved synthetic fixture is needed; this absence is not a runtime semantics claim.

The seven manually read files in items 1–5 and 7–8 were subsequently copied with explicit owner authorization into local `fixture_sources/`, preserving original relative paths and raw bytes. Seven source/copy SHA256 comparisons passed; total payload size is 7,195 bytes. `fixture_sources_manifest.json` records cases/nodepaths/hashes. `nontransitive`, `require`, `colors`, and `explicit priority3` remain explicitly uncovered. The selected root collision=no plus aboveHt=2 alone does **not** establish acceptance of nonzero-placement with nonzero-height; that combined behavior is also uncovered. No expectations or protocol fixtures were created.

`fixture_candidates.json` additionally lists bounded paths/hashes for inline p2, include, spline and polygon. Candidate selection takes the first 250 category occurrences plus all mall occurrences, then prefers small mall files; it is not an exhaustive optimal-fixture search.

## Scanner audit and exclusions

The lexer handles comments and quoted strings before declaration/block tokens. Self-tests passed for fake declarations in comments and unrelated strings, actual typed fields, include recognition, nested nodepaths, unclosed blocks and unmatched closing braces. Seven raw files above were manually read and matched their selected metadata.

`issues=[]` is only a scanner finding: zero observed brace/encoding/inventory-change issues. The scanner does **not** validate complete BLK syntax, numeric grammar, duplicate keys, suffix semantics, include expansion, references, inherited effective type/placement, shear, or importer support. Thus 26,089 scanned files is **not** 26,089 parser/import successes, and this receipt supplies no Blender/UE/runtime acceptance numbers.

Lexical searches overcount real declarations: place_type 1716 vs 1710, ignoreParentInstSeed 206 vs 190, rot_y 387 vs 365, include 258 vs 257. Complete lexical/comment tables are preserved in `inventory.json`. A comment count cannot simply be subtracted because a file can contain both comments and real declarations.


## Published evidence and local-only payloads

This inventory was explicitly authorized outside the production slice queue. It does not start S6.1 or S6.2. Its source block paths (`$.node[...]`) are BLK structural paths, **not** canonical MH resolver NodePaths or new random-stream keys.

Metadata archived with this note:

- [Capture summary](evidence/dagor_inventory_20260828/inventory.json)
- [All-depth nontransitive candidate audit](evidence/dagor_inventory_20260828/nontransitive_nested_nodes.json)
- [The 22 mall gameObj names, with source examples](evidence/dagor_inventory_20260828/mall_gameobj_names.json)
- [Seven local raw candidates: path/hash/case manifest](evidence/dagor_inventory_20260828/fixture_sources_manifest.json)
- [Frozen inventory script](evidence/dagor_inventory_20260828/inventory.py.txt)
- [Frozen focused audit script](evidence/dagor_inventory_20260828/candidate_audit.py.txt)

The scripts are diagnostic evidence, **not production tools or acceptance gates**. They are archived as text, byte-identical to the local run, with a scoped LF rule to preserve evidence hashes. Their input/output roots deliberately remain pinned to the authorized run. To inspect/reproduce that run, the original names were `inventory.py` and `candidate_audit.py` in `E:/MimirComposite_DagorInventory_20260828_R2`; the focused audit imports the first script. They must not be run over a production source root as if they were the importer.

The full 26k manifest, full gameObj occurrence catalogue, broad fixture candidate catalogue and raw `fixture_sources/` remain in that local evidence directory. No proprietary source payload, expected transform, resolved signature or new golden was added to this PR. Candidate availability is not import/round-trip acceptance; all uncovered cases above remain open for S6.2 verification.
