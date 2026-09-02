> Status: NORMATIVE · Architecture version: Recipe Model v2 · Supersedes: docs/archive/README_pre_d0.md

# MH_blender_bridge

> **АКТИВНЫЙ НОРМАТИВ** (owner, 2026-09-02, после внешнего аудита):
> [`KICKOFF_PROMPT.md`](KICKOFF_PROMPT.md) v2 — роль исполнителя, программа
> срезов, гейты; [`docs/16_recipe_model.md`](docs/16_recipe_model.md) — ADR
> модели «рецепт + исполнитель» (Status PROPOSED до R2b); этот `README.md`;
> справочник Source Protocol v5 —
> [`docs/10_source_protocol_v5_plan.md`](docs/10_source_protocol_v5_plan.md).
> Полный список — [`docs/NORMATIVE_INDEX.md`](docs/NORMATIVE_INDEX.md).
> `docs/archive/` — HISTORY, для реализации не используется;
> `docs/receipts/` — история исполнения срезов, квитанция не равна owner
> acceptance и не содержит нормативных требований; `docs/reference_notes/` —
> исследования и внешний аудит, не норматив.

DCC-driven composite pipeline: Blender (`addon/mh4blend`) публикует чистые
source-файлы, UE 5.7.4 plugin (`ue/MimirComposite`) импортирует их, размещает
композиты в уровне и передаёт результат в runtime. Модель размещения
заимствована у Dagor composit; identity и транспорт заданы Source Protocol v5.

## Три плоскости

> Dagor-подобный быстрый preview-исполнитель + Mimir-подобный строгий proof на
> границах.

| Плоскость | Что делает | Что ей запрещено |
|---|---|---|
| **Preview** | компиляция рецепта, выбор по сидам, материализация в пулы, заглушки, async-загрузка | хэши источников, full-closure proof, ожидание компиляции, чтение Asset Registry тегов |
| **Proof** | full closure, receipt freshness, `ClosureHash`/`ResolvedSignature`, admission runtime-снапшота, build preflight, export | блокировать загрузку карты или preview |
| **Source** | инкрементальный индекс файлов, targeted reimport, background freshness | парсить FBX в скане, делать FullScan на targeted reimport |

```text
Имя файла определяет identity; UUID нет.
.composite v5: parent-local T/R/S, random-узлы с весами, .placement-профили.
Seed и AppearanceSeed — явные, принадлежат размещению в UE, от позиции не зависят.
Один mh.random_stream:1 (потоки от пути узла) строит один план reference resolver.
Рецепт компилируется один раз на ассет; инстанс хранит только
  (asset, Seed, AppearanceSeed, transform, [NodeOverrides с R6]).
Preview: чистая материализация, endpoint по детерминированному пути,
  identity-admission один раз на ключ, ненайденный лист — заглушка.
Proof: full closure и SourceHash/AppliedHash только в точках выхода
  (PreSaveWorld, build preflight, runtime snapshot, export/Break).
Реимпорт меша с тем же интерфейсом композит не трогает;
  child-рецепт не перекомпилирует родителей.
Подписи остаются proof-артефактами и перестают быть состоянием актора.
```

## Документы и политика

| Файл | Роль |
|---|---|
| `KICKOFF_PROMPT.md` | активный промпт исполнителя: программа D0a → M0 → R0 → R1 → S0–S2 ∥ → R2a → R2b → R2c → R3 → R4 → R5 → R6 → R7 (→ R8), гейты, OPEN-R |
| `docs/16_recipe_model.md` | ADR модели: плоскости, слои, протокол обновлений, точки выхода, запрещённые утверждения, удалённые сущности кода, OPEN-R-1…6 |
| `docs/10_source_protocol_v5_plan.md` | протокольный справочник v5 (не меняется программой R) |
| `docs/NORMATIVE_INDEX.md` | индекс активных документов; проверяется CI |
| `docs/reference_notes/` | разбор Dagor composit, внешний аудит `external_audit_recipe_model_20260902.md`, dag4blend, корпус ассетов |
| `docs/receipts/` | квитанции срезов; полевой протокол замеров — `m0_perf_instrumentation.md` §6 |
| `docs/archive/` | HISTORY: планы 00–09, срезы 11–15, ADR, amendments, аудиты, `QUESTIONS.md`, proposals, spikes, README и KICKOFF v1 до D0a |

Документальная политика (KICKOFF §7): проверяется **нормативный статус**, не
лексика. Active-документ начинается с `Status: NORMATIVE · Architecture
version: Recipe Model v2 · Supersedes: …`; архивный — `Status: HISTORY · Do
not use for implementation · Superseded by docs/16_recipe_model.md`.
Запрещены утверждения «freshness актора определяется подписью», «карта
обязана построить proof до первого кадра», «реимпорт меша требует rebuild
актора»; сами термины `ClosureHash`/`ResolvedSignature` разрешены.
Лексический ноль — только для удалённых сущностей кода (16 §7.2). Гейт
каждого PR:

```bash
python tools/check_normative_docs.py
```

Следующий срез не начинается до owner merge предыдущего. PR мержит только
owner; Engine и `reference/` не изменяются.

## Состояние реализации

Смержено в `main`: Source Protocol v5 S0–S6 (codecs, Blender random
authoring, closure export, UE editor random, runtime-мост
`AMHRuntimeCompositeActor`), прямой экспорт dag4blend-сцены S6.1 как field
candidate (PR #30), перф-срезы U0a/U0c/U5/U7 и инструментация M0
(`mh.PerfTrace`, отчёты `MH_PERF_MAPLOAD` / `MH_PERF_STARTUP_SCAN` /
`MH_PERF_REIMPORT`, PR #60).

Программа Recipe Model v2 начинается срезом D0a (документы, эта редакция).
До R2b код редакторского слоя `Composite/` соответствует прежней модели
размещений; расхождение кода с ADR — ожидаемое переходное состояние.

## Структура репозитория

```text
KICKOFF_PROMPT.md   # активный промпт исполнителя (v2)
docs/               # 10 (протокол), 16 (ADR), NORMATIVE_INDEX, archive/, receipts/, reference_notes/
reference/          # read-only reference material (dag4blend 2.12.0, патчи, fixtures)
tools/              # build/verification tooling (addon zip, canonical, parity probes, doc CI)
golden/             # cross-host fixtures и expected reports
addon/mh4blend/     # Blender Extension
ue/MimirComposite/  # UE plugin: MimirCompositeRuntime, MimirCompositeEditor, MimirCompositeTests
```

## Blender Extension

Сборка и установка:

```bash
python tools/build_addon_zip.py
```

`dist/mh4blend-<version>-windows-x64.zip` устанавливается через Blender
**Get Extensions → Install from Disk**; пакет содержит pinned `xxhash`.
Старый script-addon перед включением Extension нужно отключить: оба
регистрируют одни и те же операторы `mh.*`.

Панель `3D Viewport > N > MH`: Mesh FBX (import/export), Composites
(Import / Export Composite / + Composite Closure / Include All Stuff),
Materials, Misc (Copy All Textures to Project, Remap All Texture Paths).
Source Root задаётся в `MH > Project > Source Root` и должен совпадать с UE
`Project Settings > Plugins > Mimir Composite > Source Root`.

Прямой экспорт импортированной dag4blend-сцены: выбирается корневая
коллекция, три команды Composite читают native MH или dag4blend-форму через
read-only диспетчер. На свежем CDK-импорте рабочий режим — **Export Composite
Include All Stuff**; текстуры заранее вносит художник командами Copy/Remap.
Сценовый адаптер — частичная совместимость, не lossless BLK round-trip
(10 §13.13).

Python-проверки без Blender (Python 3.11):

```bash
python -m pip install -r requirements-dev.txt
python -m pytest tests/ -q
```

`*_bpy.py` запускаются внутри Blender 4.5 LTS.

## UE-плагин: сборка, тесты, диагностика

Плагин собирается stock UE 5.7.4; test-host `.uproject` не модифицируется,
редактор запускается с `-EnablePlugins=MimirComposite`. Гейты каждого
C++-среза — KICKOFF §9: guarded build, `BuildPlugin -StrictIncludes` без
unity/PCH, force-unity без adaptive unity, `git diff --check`, полный NullRHI
`Automation RunTests Mimir` с `-MHGoldenRoot=<repo>/golden` (0 failed),
red-first логи, с R2a — `RecipeShadowParityTest`, `mh.PerfTrace 1` до/после
на собственном host исполнителя, документальный CI `check_normative_docs.py`.

Automation-тесты `Mimir.*` ищут `golden/` в порядке:
`-MHGoldenRoot=<repo>/golden` → переменная окружения `MH_GOLDEN_ROOT` →
`<plugin>/../../golden`. Из UI: **Tools → Session Frontend → Automation**,
фильтр `Mimir`. Headless:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "<path>\HostProject.uproject" `
  -EnablePlugins=MimirComposite `
  -MHGoldenRoot="<repo>\golden" `
  -ExecCmds="Automation RunTests Mimir; Quit" `
  -unattended -nop4 -nosplash -nullrhi -stdout -log
```

Перф-замеры: в консоли `mh.PerfTrace 1` (агрегаты) или `2` (плюс verbose
top-5); отчёты `MH_PERF_*` — в логе. Полевой протокол owner —
`docs/receipts/m0_perf_instrumentation.md` §6.

Commandlets (`-run=<Name>`, `-EnablePlugins=MimirComposite -unattended
-nop4 -nullrhi`): `MHAnalyzeSources` (анализ source root против индекса;
`-root` необязателен, если Source Root сохранён в Project Settings; exit code
1 при `MH_E_*`), `MHScanSources`, `MHImportSources`, `MHVerifyComposites`,
`MHVerifyMaterials`, `MHValidateNames`, `MHFbxDump` (слепок FBX,
`mh.fbxdump:1`). Отчёты пишутся только под `Saved/Mimir`.

```powershell
UnrealEditor-Cmd.exe <Project.uproject> -run=MHAnalyzeSources `
  -root="<absolute source root>" -unattended -nop4 -nosplash -nullrhi
```

Индекс `<UnrealProject>/Saved/MimirBridge/ProjectIndex.sqlite` — rebuildable
проекция, пишет только UE; удаление без потерь.

Runtime parity smoke (Editor = PIE = packaged) воспроизводится
`tools/s6_runtime_parity.py` и `tools/setup_s6_runtime_host.ps1`.

### Полевые тесты в UI редактора

- **Импорт `.composite`:** обычный **Import** в Content Browser (или
  drag&drop); фабрика создаёт read-only `UMHCompositeAsset`; **Reimport**
  обновляет его in place.
- **Размещение:** `AMHCompositeActor` с `Seed` / `AppearanceSeed`;
  контекстное меню — Reseed / Reseed Appearance, Copy/Paste Seed, Lock Seed,
  Keep Seed on Duplicate, Individual/Equal для мультивыделения, Show Decision
  Trace, Break Composite.
- **MH Source:** startup — только freshness-скан с отчётом в Message Log
  «N resource(s) differ… Run MH Source → Import Changed»; импорт и rebuild —
  явной командой.

UI-подписи, кнопки и логи плагина — только на английском языке (owner).
