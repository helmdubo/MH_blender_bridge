# MH_blender_bridge

DCC-driven composite asset pipeline: Blender → clean versioned source files →
UE5. Authoring model inspired by Dagor composites; identity and transport are
defined by **MH Source Protocol v2: Clean Sources**.

Active documents:

- [`docs/05_source_schema_v1.md`](docs/05_source_schema_v1.md) — normative v2
  contract (filename retained for stable links);
- [`docs/ADR_V2_passport_first.md`](docs/ADR_V2_passport_first.md) — passport,
  hashes, stateless writer and reader conflicts;
- [`docs/AMENDMENT_combined_lod_fbx.md`](docs/AMENDMENT_combined_lod_fbx.md) —
  normative Combined-LOD profile;
- [`docs/04_source_workflows.md`](docs/04_source_workflows.md) — Blender/UE UX;
- [`docs/06_final_v1_plan.md`](docs/06_final_v1_plan.md) — v2 implementation
  gates (historical filename).

Frozen Source Schema v1 SHA
`d52520c47544a6e36b3bac32b16237ad670abb20` — только migration baseline.
Production runtime не имеет manifest/uid8/dual-read fallback.

## Clean Sources v2 at a glance

Source tree содержит только primary payload:

```text
wall_a.mesh.fbx
wall_a.material
garage_set.composite
```

UID suffix отсутствует. Static mesh identity/metadata находятся в embedded FBX
passport, material/composite self-identify in-file. UE Ledger and optional
Blender Import Composite cache живут вне дерева. `export_manifest.json`,
registry, sidecars, transaction markers и cache внутри source tree запрещены.

Clean filename — человеческая подпись, resolve всегда идёт по полному UID.
Same name в разных folders легален. Collision одного clean filename с другим
UID в одном folder блокируется и требует Rename/Fork/Cancel; mtime winner и
silent overwrite отсутствуют.

## Repository layout

```text
docs/               # contract, ADR, workflows, plans, Decision Log
reference/          # dag4blend 2.12.0, read-only patterns only
tools/              # golden/migration/build tooling
golden/             # fixtures and expected semantic reports
addon/mh4blend/     # Blender Extension
ue/MimirComposite/  # UE plugin stages
```

## Blender Extension

Build:

```bash
python tools/build_addon_zip.py
```

Install `dist/mh4blend-<version>-windows-x64.zip` through Blender
**Get Extensions → Install from Disk**. The Extension package includes pinned
`xxhash`; manual `pip install xxhash` in Blender is not required. Disable/remove
the old script-addon before enabling the Extension, because both register the
same `mh.*` operators.

Target UX is one N-panel tab **MH** with sections:

- **FBX Export** — Collection, Directory, `Export Materials` default ON,
  Export FBX;
- **Composites / Import** — `.composite` path, recursive import always ON;
- **Composites / Export** — Collection, Directory, Export;
- **Materials** — Material, first-export Directory, Export;
- **Source Tools** — Actualize Texture Paths and migration.

There is no Bundle Export.

Every explicit Blender Export always writes the requested target(s): collision
guard, temporary sibling, atomic replace, exit. It never hash-skips, scans the
source root, computes a diff, or updates reader state. Blender builds an
optional resolver cache silently on first Import Composite only. UE performs
the diff at startup (silent auto-import by default, optional prompt) and in its
watcher by comparing payloads with the Ledger. Therefore a no-op Export rewrites
the source file but is classified by UE as `NO_CHANGE`.

## Combined-LOD

Author in Blender using the dag-compatible hierarchy:

```text
asset.lods
  asset.lod00
  asset.lod01
```

All levels are exported into one `asset.mesh.fbx`. Each mesh FBX node carries
integer `mh_lod_level`; node names do not determine semantics. Passport
`lod_levels` declares the full set. `mh.meshser:2` covers every level plus
UCX/SOCKET auxiliary payload, so editing any level/collision rewrites the one
FBX and produces one resource `UPDATE_GEOMETRY`.

## Materials and textures

With dag4blend enabled, exporter reads `Material.dagormat` shader, params and
`tex0…tex15`. A normal Blender material remains valid and exports as
`rendinst_simple` with empty params/textures.

Texture resolution uses:

```text
exact path -> unique basename under texture_root -> unresolved
```

Unique basename can actualize the path in `.material`; ambiguous basename is
reported for user choice. Textures are never copied by FBX/Material export.
`Export Materials=OFF` writes no material payload.

## Composite import

`.composite` v2 is the only source of graph nodes. Recursive child composites,
found FBX and materials are resolved by UID. Blender creates one definition
Collection per ResourceUID and Collection Instance Empties for placements.
Missing resources remain visible placeholders with stable NodeUID/ResourceUID;
**Resolve Missing** fills the same data-blocks later.

FBX never reconstructs composite hierarchy.

## Legacy migration

Legacy manifests, uid8 filenames and per-file `lods[]` are read only by
one-shot migration tooling. Migration performs full no-write preflight, embeds
identity/passports, converts composites to v2, re-exports Combined-LOD, renames
payloads to clean names, validates, then removes manifests. Production resolver
does not call legacy codecs.

The transitional external helper `tools/migrate_per_file_lods.py` remains for
old local per-file LOD exports; its backup/restore receipt is a migration aid,
not a runtime reader. Prefer full migration/re-export from the Blender scene.

## Development checks

Python 3.11 and Blender 4.5+ are the target development baseline.

```bash
python tools/run_all.py
python -m pytest tests -q
```

Dag4blend RNA compatibility:

```bash
blender -b --factory-startup --python-exit-code 1 \
  -P tools/check_dag4blend_compat.py
```

The repository is currently being moved through the v2 gates in
`docs/06_final_v1_plan.md`. Contract acceptance does not by itself prove the
installed ZIP or UE plugin complete; large slices require external audit and
the final field acceptance belongs to the owner.

## UE-плагин (этап C): тесты и диагностика

Плагин `ue/MimirComposite` собирается stock UE 5.7.4; test-host `.uproject` не
модифицируется, поэтому редактор запускается с `-EnablePlugins=MimirComposite`.
Automation-тесты `Mimir.*` ищут каталог `golden/` репозитория в порядке:
`-MHGoldenRoot=<repo>/golden` → переменная окружения `MH_GOLDEN_ROOT` →
`<plugin>/../../golden` (плагин запущен прямо из чекаута).

Из UI: запустите редактор (при необходимости один раз выполните
`setx MH_GOLDEN_ROOT "<repo>\golden"`), затем **Tools → Session Frontend →
Automation**, фильтр `Mimir`, Start Tests.

Headless (PowerShell):

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "<path>\MimirHead_portfolio.uproject" `
  -EnablePlugins=MimirComposite `
  -MHGoldenRoot="<repo>\golden" `
  -ExecCmds="Automation RunTests Mimir; Quit" `
  -unattended -nop4 -nosplash -nullrhi -stdout -log
```

Диагностический слепок FBX (`mh.fbxdump:1`, вне frozen-контракта):

```powershell
& "...\UnrealEditor-Cmd.exe" "<path>\MimirHead_portfolio.uproject" `
  -run=MHFbxDump "<path>\model.fbx" --full `
  -EnablePlugins=MimirComposite -unattended -nop4 -stdout `
  -abslog="<path>\fbxdump.log"
```

Слепок иерархии `.composite` v2 (кодек + resolver Clean Sources v2):

```powershell
& "...\UnrealEditor-Cmd.exe" "<path>\MimirHead_portfolio.uproject" `
  -run=MHCompositeDump "<path>\scene.composite" -root="<source_root>" `
  -EnablePlugins=MimirComposite -unattended -nop4 -stdout `
  -abslog="<path>\compositedump.log"
```

Без `-root` печатается иерархия одного файла; с `-root` commandlet сканирует
payload'ы под `source_root`, рекурсивно раскрывает `composite_ref` и печатает
resolve-статус каждого ресурса (unresolved / duplicate / divergent / cycle).
В редакторе `.composite` v2 импортируется фабрикой в read-only
`UMHCompositeAsset` (Content Browser → Import).

Reader-анализ source root против Ledger (классификация `07` §4):

```powershell
& "...\UnrealEditor-Cmd.exe" "<path>\MimirHead_portfolio.uproject" `
  -run=MHAnalyzeSources -root="<source_root>" `
  -ledger="<path>\ledger.json" -writeledger="<path>\ledger.json" `
  -report="<path>\analyze.json" `
  -EnablePlugins=MimirComposite -unattended -nop4 -stdout `
  -abslog="<path>\analyze.log"
```

Печатает строку на каждый ResourceUID (`CREATE`, `UPDATE_GEOMETRY`,
`UPDATE_DESCRIPTOR`, `UPDATE_PROPERTIES`, `MOVE`, `NO_CHANGE`,
`NO_CHANGE_EXTERNAL`, `REMOVE`, `BLOCKED`), затем предупреждения и ошибки.
Без `-root` берётся `SourceRoot` из **Project Settings → Plugins → Mimir
Composite**; если не задан ни там, ни в аргументах — usage и exit code 2.
`-ledger` читает снимок Ledger, `-writeledger` пишет продвинутый (строки
`NO_CHANGE_EXTERNAL`, `REMOVE` и `BLOCKED` не продвигаются никогда),
`-report` — JSON-отчёт `mh.analyze_sources:1`. Exit code 1 при любом `MH_E_*`.
Снимок Ledger — reader state: держите его под `Saved/`, а не в `source_root`,
и не путайте с editor-ассетом `<content_root>/_MH/Ledger`.

### Полевые тесты в UI редактора

- **Импорт `.composite`:** обычный **Import** в Content Browser (или
  drag&drop файла). Фабрика зарегистрирована на расширение `composite`,
  редактор сам выбирает наш импортёр и создаёт read-only `UMHCompositeAsset`;
  правый клик по ассету → **Reimport** обновляет его in place.
- **Tools → Mimir → Mimir FBX Dump…** — выбрать FBX: паспорт Carrier B
  (identity либо причина карантина) уходит в Message Log «Mimir», а
  канонические `*.summary.json` / `*.full.json` — в `Saved/MimirDumps/`.
- **Tools → Mimir → Mimir Composite Dump…** — выбрать `.composite`, затем
  каталог `source_root` (Cancel = слепок одного файла): иерархия, resolve-
  статусы, дубликаты/divergent/циклы — в Message Log «Mimir».
- Кастомный импорт геометрии `.mesh.fbx` в UStaticMesh (наш `FMHFbxBackend`,
  Combined-LOD, слоты из паспорта) — gate C2; до него FBX в Content Browser
  импортируется штатным UE-импортёром.
