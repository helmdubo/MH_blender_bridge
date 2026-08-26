# MH_blender_bridge

> **ACTIVE AUTHORITY: MH SOURCE PROTOCOL V5.** Норматив —
> [`docs/10_source_protocol_v5_plan.md`](docs/10_source_protocol_v5_plan.md),
> порядок срезов — [`docs/11_v5_agent_slices.md`](docs/11_v5_agent_slices.md).
> V5-S0…V5-S2 приняты owner'ом; текущая ветка реализует V5-S3 и остаётся
> кандидатом до отдельного owner review/merge.

## Source Protocol v5 в одном экране

```text
Имя файла определяет identity; UUID нет.
v5 меняет только .composite и добавляет .placement.
.composite начинается с "v": 5; migration/dual-read нет.
World(node) = World(parent) × Local(node); group — настоящий transform.
random хранит ordered weighted options; source closure обходит все options.
Seed int32 живёт только на AMHCompositeActor; в Blender seed отсутствует.
Один mh.random_stream:1 строит один FMHResolvedCompositePlan.
Editor preview = Break = runtime = PIE = packaged = cook по одному plan.
Shear блокируется на Dagor import, Blender export и UE compile.
```

Payload'ы `.material`, `.mesh.fbx`, textures, name-keyed identity, Project
Resource Index и applied state сохраняют v4-контракты без version fields.
Новый `<name>.placement` имеет kind `placement_profile` и `"v": 1`.

## Активные документы и gate

- [`docs/10_source_protocol_v5_plan.md`](docs/10_source_protocol_v5_plan.md) —
  полный ратифицированный норматив v5;
- [`docs/11_v5_agent_slices.md`](docs/11_v5_agent_slices.md) — инварианты,
  последовательные V5-S0…S7 и parked follow-up slices;
- [`docs/QUESTIONS.md`](docs/QUESTIONS.md) — история вопросов и owner-решений;
- `docs/receipts/` — квитанции срезов; automated checks не равны owner
  acceptance.

08/09 получают v5 supersede/fate banners и после ratification остаются
историей v4. Следующий срез не начинается до owner merge предыдущего. PR мержит
только owner; Engine и `reference/` не изменяются.

## Текущее состояние реализации

V5-S1 зафиксировал Python-reference `mh.random_stream:1`; принятый V5-S2
заменил `.composite` на строгий v5 codec без dual-read, добавил `.placement`
v1, parent-local T/R/S, source-only profile carrier/index edge, общий 8-ULP
predicate и бит-идентичный C++ random resolver/plan. Кандидат V5-S3 добавляет
Blender typed random/profile-authoring, четыре служебные сцены, три geometry
load mode, transactional reuse/refresh и два Dagor→MH пути с lossless
`include`→`.placement`. Seed в Blender по-прежнему отсутствует.

Python regression:

```bash
python -m pytest tests/ -q
```

Каждый C++-срез проходит stock UE 5.7.4 guarded build,
`BuildPlugin -StrictIncludes` без unity/PCH, force-unity без adaptive unity и
`Automation RunTests Mimir`. Engine не форкается.

## Структура репозитория

```text
docs/               # v5 freeze/slices/questions/receipts + history
reference/          # read-only reference material
tools/              # build and verification tooling
golden/             # cross-host fixtures and expected reports
addon/mh4blend/     # Blender Extension
ue/MimirComposite/  # UE plugin
```

---

## Архивное README Source Protocol v4

> **SUPERSEDED BY SOURCE PROTOCOL V5 UPON OWNER MERGE OF V5-S0.** Раздел ниже
> сохраняет прежний top-level README v4. Он описывает текущую production-
> реализацию на freeze-ветке, но не является v5 normative input.

> **ACTIVE TARGET: MH SOURCE PROTOCOL V4.** Единственный норматив —
> [`docs/08_source_protocol_v4_plan.md`](docs/08_source_protocol_v4_plan.md).
> Порядок реализации и definition of done —
> [`docs/09_v4_agent_slices.md`](docs/09_v4_agent_slices.md). S0–S5 приняты
> и смержены; S6 добавит startup scan, watcher, массовый импорт и финальный UX.

## Source Protocol v4 в одном экране

```text
Имя файла определяет identity. Расширение определяет тип.
Папка — только организация и текущее расположение.
StaticMesh — односторонне генерируемый asset (Blender -> UE).
Material и Composite — двусторонние JSON через explicit overwrite Publish.
Project Resource Index — rebuildable кэш, не authority.
Applied state живёт внутри соответствующего UE asset.
Дубликат имени одного kind не выбирается автоматически.
Rename — сознательный breaking change.
UUID не существуют нигде.
```

`ResourceKey = Kind + LogicalName`, где logical name соответствует
`[a-z0-9_]+` и получается снятием полного составного расширения:

```text
garage_a.mesh.fbx       -> static_mesh:garage_a
m_stucco.material       -> material:m_stucco
garage_type_a.composite -> composite:garage_type_a
brick_a_tex_d.png       -> texture:brick_a_tex_d
```

Source-дерево может иметь любую структуру папок. Перемещение файла сохраняет
identity; rename означает `DELETE + CREATE`. Одинаковый logical name разных
kinds допустим. Дубликаты одного kind блокируют resource и dependents — scanner
не выбирает победителя по mtime, размеру или порядку папок.

## Активные документы

- [`docs/08_source_protocol_v4_plan.md`](docs/08_source_protocol_v4_plan.md) —
  единственный активный норматив;
- [`docs/09_v4_agent_slices.md`](docs/09_v4_agent_slices.md) — инварианты,
  срезы S0–S6 и сводные acceptance-тесты;
- [`docs/QUESTIONS.md`](docs/QUESTIONS.md) — открытые неоднозначности с
  временными fail-closed правилами;
- `docs/receipts/` — квитанции выполненных v4-срезов.

Документы `00–07`, `ADR_*`, `AMENDMENT_*`, прежние `ROADMAP` и policy-выводы
`RISK_RESULTS` исторические или частично superseded. Выжившие части `07` и двух
amendment перечислены в их верхних баннерах и в 08 §12.

## Контракты payload

- `*.mesh.fbx` — обычный FBX без MH passport/custom properties. Один файл
  содержит все dense LOD; mesh nodes классифицируются по `_lodNN`, collision —
  по `UCX_`/`_cls_phys|trace|both`, sockets — по `SOCKET_`, остальные null nodes
  передают группы. Parent closure проверяется fail-closed. Изменение raw hash
  вызывает полный in-place rebuild `UStaticMesh`.
- `*.material` — лаконичный JSON: форма `class` несёт `textures` и `params`, а
  форма `library` задаёт ссылку на library material; обе формы без
  schema/version/mode/UID. Publish полностью перезаписывает source через sibling
  tmp, read-back и atomic replace; writer не делает diff.
- `*.composite` — JSON-дерево `mesh|actor|composite|group` с transform и без
  material information/UID. Publish имеет ту же full-overwrite семантику;
  циклы и unresolved references блокируют resource.
- Текстуры — полноправный kind: reference в `.material.textures` — только
  logical name без расширения; резолв — по ResourceKey `texture:<name>` в
  границах source-дерева со стандартной duplicate-policy, приоритетов
  форматов нет. Allowlist image extensions и коды ошибок заданы 08 §5
  (решение [`OPEN-V4-2`](docs/QUESTIONS.md#open-v4-2--canonical-texture-reference-и-image-extensions)).

UE — единственный писатель rebuildable индекса
`<UnrealProject>/Saved/MimirBridge/ProjectIndex.sqlite`. Blender индекс не
читает и не пишет. Генерируемые UE paths детерминированы kind и logical name и
не зависят от source-папки.

### Ручная индексация до S6

После S5 интерактивных кнопок `Scan Project` и автоматического startup watcher
ещё нет: это scope S6. Для текущего ручного full scan сначала задайте один и
тот же абсолютный Source Root:

- Blender: `3D Viewport > N > MH > Project > Source Root`;
- UE: `Project Settings > Plugins > Mimir Composite > Source Root`.

Затем при закрытом Editor запустите:

```powershell
UnrealEditor-Cmd.exe <Project.uproject> -run=MHAnalyzeSources `
  -root="<absolute source root>" -unattended -nop4 -nosplash -nullrhi
```

Параметр `-root` можно опустить, если `Source Root` сохранён в Project
Settings. Команда перестраивает/обновляет `ProjectIndex.sqlite` и возвращает
ненулевой exit code при блокирующих `MH_E_*`. Blender ничего дополнительно
индексировать не должен: он только атомарно публикует source-файлы.

## Реализация и проверка

Работа идёт последовательно отдельными PR: S0 documentation → S1 purge legacy
UID/passport → S2 materials → S3 composites → S4 index → S5 StaticMesh importer
→ S6 watcher/commandlets/UX. Каждый срез имеет ветку `v4/s<N>-*` и квитанцию
`docs/receipts/v4_s<N>.md`; исполнитель не мержит PR самостоятельно.

Текущий main включает закрытые Material/Composite v4 codecs, Project Resource
Index и direct-FBX StaticMesh importer S5. До S6 full scan запускается вручную,
массового `Import Changed`, watcher и финальных UE toolbar-команд ещё нет.

Python-проверка без Blender:

```bash
pip install -r requirements-dev.txt
python -m pytest tests/ -q
```

`*_bpy.py` запускаются внутри Blender 4.5 LTS. UE-плагин проверяется на stock UE
5.7.4 guarded build с `-EnablePlugin=MimirComposite -NoEngineChanges -NoUBA`,
`BuildPlugin -StrictIncludes` без unity/PCH и `Automation RunTests Mimir`.
Engine не модифицируется и не форкается.

## Структура репозитория

```text
docs/               # v4 contract, slices, questions, receipts, history
reference/          # read-only reference material
tools/              # build and verification tooling
golden/             # fixtures and expected reports
addon/mh4blend/     # Blender Extension
ue/MimirComposite/  # UE editor-only plugin
```

---

## Архивное README Source Protocol v2

> **SUPERSEDED BY SOURCE PROTOCOL V4.** Весь оставшийся body сохранён без
> удаления как историческое описание v2 и удалённого в S1 tooling. Он не
> является active contract или implementation input; любые UID/passport/
> Ledger/UE→FBX утверждения ниже ненормативны.

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

- **Mesh FBX** — Import File либо Collection/Folder/Export Materials/Export;
- **Composites** — Import или Export source-composite;
- **Materials** — Material, first-export Folder и точечные v4 overrides;
- **Misc** — двухфазные Copy All Textures to Project и Remap All Texture
  Paths внутри той же панели MH.

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
`tex0…tex15`; содержимое, представимое closed-грамматикой v4, не нужно
дублировать в MH-полях. Непредставимый тип блокирует material fail-closed.

Misc workflow переносит внешние текстуры в source-проект без изменения
идентичности:

```text
<external>/assets/<tail> -> <Project Source Root>/assets/<tail>
```

Сначала **Copy All Textures to Project** атомарно копирует все непустые Dagor
slots всех материалов текущего `.blend`, сохраняя дерево ниже единственного
сегмента `assets`. Затем **Remap All Texture Paths** после полного preflight
переназначает slots на project-файлы; при ошибке изменения откатываются. Сам
FBX/Material Export текстуры не копирует. В `.material` по-прежнему попадает
только extensionless logical name, который индекс резолвит внутри Source Root.

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
  -ledger="<path>\ledger.json" -report="C1\analyze.json" `
  -EnablePlugins=MimirComposite -unattended -nop4 -stdout `
  -abslog="<path>\analyze.log"
```

Печатает строку на каждый ResourceUID (`CREATE`, `UPDATE_GEOMETRY`,
`UPDATE_DESCRIPTOR`, `UPDATE_PROPERTIES`, `MOVE`, `NO_CHANGE`,
`NO_CHANGE_EXTERNAL`, `REMOVE`, `BLOCKED`), затем предупреждения и ошибки.
Без `-root` берётся `SourceRoot` из **Project Settings → Plugins → Mimir
Composite**; если не задан ни там, ни в аргументах — usage и exit code 2.
`-ledger` читает снимок Ledger, `-report` пишет JSON-отчёт
`mh.analyze_sources:1`. Relative report path резолвится под `Saved/Mimir`;
absolute path также обязан находиться под `Saved/Mimir`. Exit code 1 при любом
`MH_E_*`. В C1 `-writeledger` намеренно отклоняется с exit code 2: Analyze/Plan
не продвигает last-applied state до успешной Execute-операции. Снимок Ledger —
reader state; его нельзя размещать в `source_root` и не следует путать с
editor-ассетом `<content_root>/_MH/Ledger`.

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
