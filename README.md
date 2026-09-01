# MH_blender_bridge

> **АКТИВНЫЙ НОРМАТИВ — ровно три файла** (ратификация owner 2026-09-02):
> [`KICKOFF_PROMPT.md`](KICKOFF_PROMPT.md) — роль исполнителя, программа
> срезов R/S, гейты; [`docs/16_recipe_model.md`](docs/16_recipe_model.md) —
> модель «рецепт + исполнитель» редакторского слоя UE; этот `README.md` —
> карта репозитория и полевые команды. Wire-контракты Source Protocol v5
> (identity, индекс, FBX, материалы, `.composite`/`.placement`, receipt, сиды)
> — справочник [`docs/10_source_protocol_v5_plan.md`](docs/10_source_protocol_v5_plan.md).
> Всё в `docs/archive/` — история под шапкой `HISTORY`; `docs/receipts/` —
> история исполнения срезов, квитанция не равна owner acceptance;
> `docs/reference_notes/` — датированные исследования, не норматив.

DCC-driven composite pipeline: Blender (`addon/mh4blend`) публикует чистые
source-файлы, UE 5.7.4 plugin (`ue/MimirComposite`) импортирует их, размещает
композиты в уровне и передаёт результат в runtime. Модель размещения
заимствована у Dagor composit; identity и транспорт заданы Source Protocol v5.

## Модель в одном экране

```text
Имя файла определяет identity; UUID нет.
.composite v5: parent-local T/R/S, random-узлы с весами, .placement-профили.
Seed и AppearanceSeed принадлежат размещению в UE; в Blender сида нет.
Один mh.random_stream:1 (потоки от пути узла) строит один план резолвера.
Рецепт компилируется один раз на ассет; инстанс хранит только
  (asset, seed, appearanceSeed, transform, nodeOverrides).
Материализация — чистая функция; листья резолвятся по детерминированному пути,
  один раз на ключ за сессию; ненайденный лист — заглушка.
Receipt (провенанс) живёт в UE asset и проверяется только в точках выхода:
  PreSaveWorld, build preflight, runtime snapshot, export/level operations.
Реимпорт меша композит не трогает; рецепт пересобирает только свои инстансы.
Editor preview = Break = PIE = packaged по одному плану.
```

## Документы

| Файл | Роль |
|---|---|
| `KICKOFF_PROMPT.md` | активный промпт исполнителя: программа R (D0, R0–R7), линия S (S0–S2), гейты, OPEN-правила |
| `docs/16_recipe_model.md` | норматив модели: слои, протокол обновлений, точки выхода, удалённые термины и греп-гейт, OPEN-R-вопросы, карта документов |
| `docs/10_source_protocol_v5_plan.md` | протокольный справочник v5 (не меняется программой R) |
| `docs/reference_notes/` | разбор Dagor composit / dag4blend / корпуса ассетов; улика |
| `docs/receipts/` | квитанции срезов; полевой протокол замеров — `m0_perf_instrumentation.md` §6 |
| `docs/archive/` | история v1–v5: планы 00–09, срезы 11–15, ADR, amendments, аудиты, `QUESTIONS.md`, proposals, spikes |

Следующий срез не начинается до owner merge предыдущего. PR мержит только
owner; Engine и `reference/` не изменяются.

## Состояние реализации

Смержено в `main`: Source Protocol v5 S0–S6 (codecs, Blender random
authoring, closure export, UE editor random, runtime-мост
`AMHRuntimeCompositeActor`), прямой экспорт dag4blend-сцены S6.1 как field
candidate (PR #30), перф-срезы U0a/U0c/U5/U7 и инструментация M0
(`mh.PerfTrace`, отчёты `MH_PERF_MAPLOAD` / `MH_PERF_STARTUP_SCAN` /
`MH_PERF_REIMPORT`, PR #60).

Программа Recipe Model (KICKOFF §5) начинается срезом D0 (документы, эта
редакция) и продолжается C++-срезами R0–R7. До их выполнения код
редакторского слоя `Composite/` соответствует прежней модели; расхождение
кода с `docs/16_recipe_model.md` — ожидаемое состояние, закрываемое срезами,
а не основание восстанавливать старую модель в документах.

## Структура репозитория

```text
KICKOFF_PROMPT.md   # активный промпт исполнителя
docs/               # 10 (протокол), 16 (модель), archive/, receipts/, reference_notes/
reference/          # read-only reference material (dag4blend 2.12.0, патчи, fixtures)
tools/              # build/verification tooling (addon zip, canonical, parity probes)
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
red-first логи, `mh.PerfTrace 1` до/после на собственном host исполнителя.

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
