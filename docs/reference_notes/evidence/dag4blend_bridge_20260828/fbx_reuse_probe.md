# V5-S6.1 direct-export FBX reuse audit

Archive note: this is an LF-normalized copy of the private probe receipt.
Original paths and SHA256 values below identify the original private files.
The adjacent archive uses names `fbx_reuse_process_one.json`,
`fbx_reuse_process_two.json`, `fbx_reuse_cross_process.json`,
`fbx_reuse_probe.py` and `fbx_reuse_compare_processes.py`.

Read-only production audit; all probe writes are confined to this diagnostic directory. No FBX normalization, semantic hash, comparer, source publication, repository mutation, or installed Blender/Engine modification was performed. Owner document 13 R2 was read completely.

## Measured result

Real `prepare_fbx_collection` → `stage_prepared_fbx` → structural FBX read-back, Blender **4.5.12 LTS** (`84afd5f785f7`), FBX 7400. Two independent factory-startup processes, three exports each: unchanged A, unchanged B, then one vertex x=1→2. The source collection has only dag4blend `type`/`name`, no MH stamps.

| Comparison | Raw FBX | parse_mesh_fbx plan | Full parsed tree |
|---|---|---|---|
| Same-process unchanged A/B, both processes | different | equal | different: CreationTimeStamp |
| Vertex-only edit | different | **equal** | different: Vertices plus timestamp |
| Unchanged A in independent processes | different | equal | different: numeric IDs/connections |

In process_one, unchanged A/B differ only at `FBXHeaderExtension/CreationTimeStamp/Millisecond`: **830→887**. The vertex edit changes the serialized vertex coordinate **100→200 cm** but leaves both `parse_mesh_fbx` output and `_mesh_authority_fingerprint` equal. Thus neither existing structure-only object can serve as content equality.

Between process_one.A and process_two.A, five parsed-tree paths differ: Documents/Document, Objects/Geometry, Objects/Model and two Connections. They contain different numeric identifiers; the timestamps happened to coincide because the processes started in parallel. Both files are 11,068 bytes.

Baseline hashes: process_one.A `ead327a2cd7bcfd58ffae3e2fd305102b151f3fed6dc151ea3192cf6b38b285b`; process_one.B `92d0ea6c1d9b88304d7cd728399d2b858c21e7d4cb6e2dc48033d0098b150b9e`; process_two.A `101a26bffcd93db0a6be52399e3bc7a76b2c9d17d4e2a48377871f0a64ae8e30`.

## Code explanation and contract boundary

- Stock `io_scene_fbx/export_fbx_bin.py::fbx_header_elements` uses `datetime.now()` for CreationTimeStamp. `encode_bin.py` fixes the separate top-level CreationTime/FileId, but not this header timestamp.
- `io_scene_fbx/fbx_utils.py::_key_to_uuid` derives numeric IDs from Python `hash(key)`; fresh processes produce different IDs. MH staging preserves the bytes verbatim.
- `parse_mesh_fbx` / `MeshImportPlan` classify names, types, parent names and material slots, not vertices, normals, UVs or transforms. `_mesh_authority_fingerprint` is a transient pointer/structure preflight guard, not content serialization.
- JSON resources already have canonical writers and exact content comparison. Exact existing-source/staged-byte equality is also safe for FBX **when equal**, but cannot satisfy unchanged-repeat acceptance with this writer.
- Document 10 §4 removes Blender mesh semantic hashing; §9 states binary kinds have no canonical extract and use raw source bytes. Document 13 R2 §5 / acceptance 8 requires unchanged unstamped inputs to reuse Source Root payloads. There is no existing approved FBX content-equivalence mechanism bridging these requirements.

**Bounded recommendation for OPEN-V5-22:** retain canonical JSON reuse; do not substitute classification/fingerprint equality for mesh content. Owner must choose whether to authorize deterministic FBX transport bytes or a narrowly specified transient full-FBX comparison with an explicit volatile-field/ID-equivalence rule. A generic tree comparison that discards timestamps and remaps IDs is technically possible, but is a **new equivalence contract**, not an existing safe helper. Unknown fields, geometry arrays, connections and writer-supported constructs would need fail-closed treatment. No such policy was invented here.

## Reproduction and evidence

Immutable snapshot commit: `5f566c7b16e36fa68e1e5ef1391675d1c5febf2d`. The snapshot's `export_fbx.py` and the actual S6.1 worktree `E:/MimirComposite_V5S6_1_Direct_Source_20260828/addon/mh4blend/scene/export_fbx.py` were hash-checked equal: `32f2a5ac0a8eadd7b24e562de3cc84b0aa6efc29728a629eef5af3323f8d6831`. Snapshot obtained with read-only `git archive`; no checkout/reset/stash/index command.

Executed twice, using labels `process_one` and `process_two`:

```text
C:/Program Files/Blender Foundation/Blender 4.5/blender.exe --background --factory-startup --disable-autoexec --python-exit-code 19 --python E:/MimirComposite_V5S6_1_DirectReuseProbe_20260828/probe_repeat_fbx.py -- <label>
```

BLENDER_USER_RESOURCES/CONFIG/SCRIPTS/DATAFILES/EXTENSIONS were all redirected to the corresponding private `user/` paths; `PYTHONDONTWRITEBYTECODE=1`. Existing label directories must not be overwritten; use fresh labels for additional probes. Both measured processes exited 0. Source Root remained empty; these were staging-only exports.

Then: `C:/Python313/python.exe E:/MimirComposite_V5S6_1_DirectReuseProbe_20260828/compare_processes.py`.

Original evidence files (relative to the private probe directory):

- `reports/process_one.json` — SHA256 `964b0d6d5eab90c5b69ed4c998f8196cc65d74d984ed489fddb789bf1422a79d`.
- `reports/process_two.json` — SHA256 `13457ecf828abc3c1d688c49d7bae7030c355c6d752a445017748b088aeb95b1`.
- `reports/cross_process.json` — SHA256 `c65c6657fa9e28575ee5e0fe498ccce71f5265562299187b01099fff9b4d6a64`.
- `probe_repeat_fbx.py` — SHA256 `3e7e290eff387329208a5f4e4a1e8739bcdb16747b8fa0ba82c502d7d6be40c1`.
- `compare_processes.py`; per-run raw FBX and parsed-tree JSON under `process_one/` and `process_two/`.

An existing unrelated `reports/process_a.json` was not used for these conclusions. All results above come from the two explicitly named fresh measured runs.
