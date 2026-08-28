# V5-S6.1 diagnostic evidence — not protocol goldens

Base: `5f566c7b16e36fa68e1e5ef1391675d1c5febf2d`, 2026-08-28.
See `../../../receipts/v5_s6_1.md` for status, measured scope and limitations.
These artifacts do not ratify a carrier or change the Source Protocol.

- `baseline_bridge_probes.py.txt` + `baseline_bridge_red.xml`: four failing
  Blender tests against unchanged base production in a detached worktree.
- `MHGameObjAdmissionAudit.cpp.txt` + `gameobj_automation.json`: three actual
  UE tests, two RED/one GREEN. `gameobj_probe_receipt.md` gives commands and
  distinguishes source/applied admission from the low-level placeholder.
- `probe_dag4blend.py.txt` + `dag4blend_datamodel.json`: real dag4blend 2.12.0
  operator calls, root-loss/local-control/omitted-name observations. The
  harness uses a private copy and intentionally disables geometry recursion;
  it is not an E2E exporter test.
- `scan_empty_ent.py.txt` + `empty_ent_inventory.json`: full-corpus declaration
  census. The script depends on the earlier diagnostic lexer, archived as
  `../dagor_inventory_20260828/inventory.py.txt`; adapt its diagnostic path
  when reproducing elsewhere. `dag4blend_probe_receipt.md` records boundaries.
- `relink_ownership_red.xml`: red candidate tests from an independent audit,
  before the ownership fixes. This is not a red run of main. Its unrelated
  publication failure records that the concurrently implemented helper was
  not yet present; that failure is not claimed as baseline evidence.

All archived text is UTF-8/LF, pinned in `.gitattributes`. In particular the
UE JSON archive is a normalized text copy, not the raw host file:

Pytest XML traceback whitespace is preserved, including trailing indentation.
The narrow `*.xml whitespace=-blank-at-eol` attribute prevents rewriting this
captured evidence just to satisfy a source-code whitespace check; hashes below
remain unchanged. It does not relax whitespace checks for code or other files.

| Artifact | SHA256 |
| --- | --- |
| raw host `Reports/index.json` | C56EAF149DE742A7EFAF7F66E2620ADAFA3B7E584FD20D1BC822F18DC3F915E5 |
| archived `gameobj_automation.json` | 14D03273CC5030DB0EA743E15A1D23F8F9B80A38AB4FEB11417288767EC3D4DD |
| `MHGameObjAdmissionAudit.cpp.txt` | 05ED8176DADCDA5977D524D7A070B9F6BEF57451C7909A1DA3A0BB7497D4E98D |
| `dag4blend_datamodel.json` | 167355cb8b2bfd0fd3f50fa503dc3c06df218b68ef76bee6617f5879b0b3b897 |
| `empty_ent_inventory.json` | 6287bdc250ca978c65bcc9888a05f3c42e5afb52b5bdb7153f04057b8bd38176 |
| `baseline_bridge_probes.py.txt` | 6A18EC52C77786935101CCB4E99B61F4831B39036938BDA5CE5120545C5B69CE |
| `baseline_bridge_red.xml` | E59F967F49739E535275BAEEC9E4F74C498BB2ADBA67E4508C9F3D9F38C8C897 |

Raw and archived UE JSON were parsed and compared structurally: identical.
No Engine/reference/installed addon was modified to obtain this evidence.
