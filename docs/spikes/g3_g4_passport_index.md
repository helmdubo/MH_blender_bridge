# G3/G4 — local passport index and payload publication spikes

> **SUPERSEDED BY SOURCE PROTOCOL V4 AS POLICY; SPIKE EVIDENCE ONLY.** Все
> passport/UID/index-architecture решения ниже ненормативны и не являются
> implementation input. Atomic-publication measurements сохранены только как
> историческая квитанция; единственный действующий контракт —
> [`../08_source_protocol_v4_plan.md`](../08_source_protocol_v4_plan.md).

> **SPIKE EVIDENCE ONLY / INDEX ARCHITECTURE SUPERSEDED.** Active Clean Sources
> v2 uses a stateless Blender writer: every explicit Export performs target
> collision guard, tmp→atomic rename and exits, with no hash-skip, diff, source
> scan or index update. Blender may build an optional lazy cache only on first
> Import Composite; UE startup/watcher compares scans with Ledger. Therefore
> all shared/local writer-index behavior and any «Source Schema v1 remains
> active» statement below are non-normative. Atomic publication/crash
> measurements remain useful evidence.
> Historical `MH_W_MISSING_MATERIAL` below maps to active
> `MH_W_MATERIAL_NOT_FOUND`; do not add the old spelling to production registry.

Historical status: **pure local half PASS; integration gate remained BLOCKED.**
The files are executable spike evidence, not production add-on code or schema.

The combined-LOD amendment supersedes the earlier per-LOD-file wording in the
passport-first ADR for this spike:

- one `ResourceUID` owns exactly one FBX payload;
- its passport declares `lod_levels: [0, ..., N]`;
- there is no file-level `lod_level` and no `lod_set` of candidate files in the
  local index;
- Model-level `mh_lod_level` enumeration and comparison with the declared list
  belongs to the selected G1 carrier reader, not to this filesystem index;
- an old passport with `lod_level` is classified
  `deprecated_per_lod_passport` with an explicit migration diagnostic and is
  not accepted as a v2 UID candidate.

## Scope and reproduction

Implemented only in:

- `tools/spikes/passport_index.py`;
- `tools/spikes/payload_publish.py`;
- `tests/test_passport_index_spike.py`;
- `tests/test_payload_publish_spike.py`.

Run from repository root with ordinary CPython:

```powershell
python -m pytest tests/test_passport_index_spike.py `
  tests/test_payload_publish_spike.py -q
```

Receipt on Windows, 2026-08-18: **15 passed**.

## G3 result — rebuild and conflicts

The disposable index inventory is:

```text
%LOCALAPPDATA%/MimirHead/MHBridge/<project_uid>/index.json
```

macOS uses `~/Library/Caches`; other platforms use `$XDG_CACHE_HOME` or
`~/.cache`. The helper requires a UUID project id. The cache is never placed in
the source tree.

Each path row records `size`, `mtime_ns`, an exact `sha256:` byte fingerprint,
parse status, parsed passport when valid, descriptor hash, quarantine state and
diagnostics. Size plus mtime is only a fast path for reusing a previously
computed exact hash; it is never presented as the payload fingerprint itself.
Changing bytes with changed stat data rehashes the payload. A changed exact
fingerprint under the same parsed passport emits
`MH_W_PAYLOAD_EXTERNAL_MODIFIED`.

Measured conflict matrix:

| State | Spike result |
|---|---|
| UID absent from prior index, one payload | `adopt_new` |
| Prior resolved path gone, one new candidate | `moved` |
| Two existing candidates, exact fingerprints equal | `duplicate_copy_warning` |
| Two candidates, fingerprints differ | hard `MH_E_DIVERGENT_REVISIONS`, no resolved path |
| Prior resolution still exists among identical copies | prior path is preserved |
| Empty-cache rebuild with identical copies | normalized-path winner, explicitly `provisional` |
| No carrier copies | `legacy_missing_passport`, dual-read/adopt required |
| Old per-file `lod_level` passport | `deprecated_per_lod_passport`, migration required |
| Malformed, carrier copies differ, or version unknown | quarantine with `MH_E_PASSPORT_INVALID` |
| Declared `lod_levels` is not contiguous `[0..N]` | malformed/quarantine |
| Referenced MaterialUID is absent from supplied material inventory | `MH_W_MISSING_MATERIAL` |

The normalized-path choice on an empty rebuild is not promoted to v2 policy.
It is recorded with this exact note:

> No prior resolution existed; identical copies were provisionally resolved
> by normalized path order. A host policy must replace this before v2 freeze.

The pure `fork_document` operation deep-copies a passport or composite, assigns
a caller-selected/new UUID, and can rewrite only explicitly selected composite
node dependencies. Tests prove the source document and unselected links remain
unchanged. UI confirmation, filesystem publication, and undo belong to later
host work.

The partial-LOD filesystem case from the withdrawn per-file model no longer
exists. This spike rejects a non-contiguous passport declaration. Detecting a
missing or duplicated FBX Model for a declared level remains a required G1
carrier-reader test and is not falsely marked green here.

## G4 result — crash semantics

`atomic_publish_bytes` uses this sequence while holding a lock for one
canonical destination path:

1. create a uniquely named sibling temp;
2. write all bytes;
3. flush and `fsync` the file;
4. `os.replace(temp, destination)`;
5. `fsync` the parent directory where the OS exposes a directory descriptor.

The lock file is stored outside the source tree under the local cache, keyed by
SHA-256 of the canonical payload path. It uses an OS file lock, so process death
releases ownership; the inert lock file may remain. There is no global index
lock and no source-tree marker.

Real `multiprocessing` probes show:

- hard process exit before replace leaves the old destination byte-exact;
- hard process exit after replace leaves the new destination byte-exact;
- two concurrent writers finish with one complete authored byte sequence,
  never a torn mixture;
- a stale index notices the changed exact payload fingerprint and rebuilds;
- deleting `index.json` loses no authority and rebuilds from payloads as
  `adopt_new`.

A pre-replace crash may leave a sibling temp. It is not authority and scanners
must ignore the reserved `.mh-tmp-` pattern. Cleanup policy is operational and
not settled by this spike. Windows does not expose the same simple directory
`fsync` primitive as POSIX; the receipt reports that limitation rather than
claiming it happened.

## Still BLOCKED — do not call G3/G4 fully green

- **Blender and UE writing the same real payload simultaneously:** BLOCKED on
  a cross-host receipt using the final Python/C++ lock implementations and the
  same canonical-path/key algorithm.
- **Real sync folder:** BLOCKED on OneDrive/Dropbox/studio-sync measurements,
  including rename propagation, conflict copies, mtime behavior and lingering
  sibling temps.
- **Combined-LOD carrier completeness:** BLOCKED on the G1 reader enumerating
  every FBX Mesh Model's `mh_lod_level` and proving exact agreement with
  passport `lod_levels` in Blender and UE.

Therefore the pure algorithms are suitable evidence for drafting v2, but they
do not authorize production migration, removal of v1 dual-read, or a claim that
the full G1–G4 gate is green.
