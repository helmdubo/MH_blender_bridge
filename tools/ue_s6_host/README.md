# Isolated V5-S6 runtime acceptance host

This is a reproducible test harness, not owner-project content or a plugin
runtime dependency. Create a fresh host with `tools/setup_s6_runtime_host.ps1`.
The script copies these files and junctions only the repository plugin. It never
modifies Engine or an existing host.

Use separate fresh host directories for the full regression suite and the
persisted cook fixture. Some historical S5 tests intentionally create transient
assets under the same frozen logical names; rerunning them in a host whose
Content already contains the persisted S6 fixture is not an isolated test.
Do not remove or overwrite owner-project assets to make that test pass.

Run these targeted Automation tests in separate Editor processes with
`-MHGoldenRoot=<repository>/golden -MHS6ParityHost -NoAssetRegistryCache`:

- `Mimir.V5.Runtime.Parity.SerializedAutomation`
- `Mimir.V5.Runtime.Parity.EditorPreview`
- `Mimir.V5.Runtime.Parity.PIE`, additionally passing `-MHS6PersistFixture`

The last run saves seven **editor** placements and applied fixture assets to
`/Game/MimirS6/RuntimeParity`, then runs real PIE. The plugin's normal PIE/cook
bridge must produce runtime actors; the harness does not pre-bake them.

All three produce separate reports in the host's `Saved/Mimir/S6`. The fixture
uses the immutable S1.1 seed set `0, 1, 2, 42, 123, 1024, 2147483647` and its
accepted synthetic-domain receipt hashes. Stock cube copies supply real cooked
mesh geometry; these are test visualization payloads, not new golden sources.

Use stock `RunUAT BuildCookRun -build -cook -stage -pak -archive -platform=Win64
-clientconfig=Development -map=/Game/MimirS6/RuntimeParity` against this host.
For the headless smoke, pass
`-AdditionalCookerOptions="-nullrhi -nosound -NoAssetRegistryCache"`.
The last option bypasses the host's disposable discovery cache, not source
admission or the runtime input. Do not pass `-IgnoreCookErrors`.
Run the archived game executable with `-MHS6PackagedSmoke -nullrhi -unattended
-nosound`. It reads only cooked map/input/references, writes `packaged.json`
under its own `Saved/Mimir/S6`, verifies seven seeds and absence of editor-only
modules, and exits 0 on success or 1 on failure. It never reads repository
goldens or source files.

`tools/s6_runtime_parity.py` observes the Python reference independently and
compares all four UE reports with it and the immutable golden fields. Keep the
five lane reports and process/build logs separately in the slice receipt.

Negative cook: with all Editor processes closed, temporarily move only the
isolated host's generated `variant_b1_mesh.uasset` outside `Content`, cook to
a fresh `-CookOutputDir`, and require a nonzero UAT exit with the matching MH
missing-dependency diagnostic. Always restore the file and verify its original
hash in a `finally` block. Never package the failed cook's output. This resource
is unselected for seeds 0/1/2/42 but selected for the other three placements;
do not describe it as unselected for every actor in the seven-placement map.
