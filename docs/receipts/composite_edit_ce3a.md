# CE-3a (Composite Edit Mode) — режим редактора, панель Save | Cancel, locked context

Статус: **REVIEW** (близнец). Первая половина карточки CE-3 спецификации
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md`, CE-ADR-2);
выделение узлов проекции из Outliner/viewport и breadcrumbs-навигация — CE-3b.

## 1. Что сделано

- `UMHCompositeEditorMode : UEdMode` (Public/Editing) — публичный режим
  плагина, невидимый в списке режимов (`FEditorModeInfo(..., bVisible=false)`),
  регистрируется движком автоматически по CDO
  (`UAssetEditorSubsystem::RegisterEditorModes`). Подсистема активирует его
  на `GLevelEditorModeTools()` после открытия сессии+проекции под флагом
  `bCompositeEditModeV2` и деактивирует в `ResetEditSession` (guard: `Exit`,
  вызванный подсистемой, не отменяет сессию повторно; `Exit` по чужой
  инициативе — другой режим, смена уровня — отменяет сессию, draft без
  режима не живёт).
- Locked context: `IsSelectionDisallowed/IsEditingDisallowed` — выделять и
  редактировать можно только projection-актор сессии; снятие выделения
  всегда разрешено.
- Панель во viewport (`FMHCompositeEditorModeToolkit : FModeToolkit`, форма
  Level Instance Edit, решение owner 2026-09-07): иконка композита,
  `Composite Edit | root > child[ *]` (breadcrumb из цепочки вызова,
  `*` при dirty), **Save** (PrimaryButton → `MHExecuteCommitEditCompositeInteractive`,
  обычное подтверждение перезаписи) и **Cancel** (→ `RequestCancel`: при
  dirty вопрос «Discard unsaved composite changes?» Yes/No; при «No»
  сессия и режим остаются). Toolkit создаётся только при наличии toolkit
  host (headless-тесты — без UI, логика та же).
- Команды `FMHCompositeEditCommands` (`TCommands`): `CancelEdit` = Escape с
  тем же приоритетом, что у движка: пока есть выделенные акторы и chord
  совпадает с SelectNone — сначала снятие выделения; `SaveEdit` без chord
  (Enter не публикует). Регистрация/снятие в модуле.
- Dimming: show flag `EditingLevelInstance` на всех level-viewport'ах при
  входе (и в `ModeTick` для новых окон), снимается при выходе.
  Пометка примитивов проекции как editing (`PushLevelInstanceEditingStateToProxy`)
  — CE-3b (там же визуальная проверка).
- `OnRequestClose` → `RequestCancel` (Stay блокирует закрытие);
  `PreBeginPIE` → `RequestCancel` (v1: при «Stay» PIE стартует с открытой
  сессией; projection-актор editor-only и в PIE не дублируется — известный
  gap, см. §5).
- Build.cs: `EditorFramework`, `InteractiveToolsFramework`,
  `EditorInteractiveToolsFramework`.

## 2. Тесты (red `b6d5ed4`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Mode.FollowsTheSessionAndLocksTheContext` | под флагом: до Begin режим неактивен; после — активен, `GetActive`; projection-актор выделяем, другое размещение и корневой актор — нет, снятие выделения разрешено; Cancel → режим неактивен |
| `…Mode.CancelAsksWhenDirtyAndSaveApplies` | чистая сессия: `RequestCancel` уходит без вопроса; dirty: «Stay» → сессия и режим живы, вопрос задан один раз; «Discard» → ушли, источник не тронут; `RequestSave` → одно подтверждение перезаписи, опубликован именно child, сессия и режим закрыты, child несёт правку |
| `…Mode.LegacyBackendNeverActivatesIt` | без флага Begin идёт legacy-путём, режим неактивен |

Попутно починен нестабильный тест CE-2b
`…Projection.ComponentsMapToDraftNodes`: имена fixture-ассетов несут per-run
суффикс, поэтому выбор random-узла (mesh A | empty) меняется от запуска к
запуску; хэндл структурного random-узла есть всегда, а лист выбранной
опции — только когда выиграл mesh. Прежнее утверждение «ровно одно из
двух» падало при выборе mesh (`CE3A_FULL.log`: 247/1).

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`b6d5ed4`) | `CE3A_RED_TEST.log`: Fail ×2 (`LegacyBackendNeverActivatesIt` — негативный, зелёный уже на stub) |
| GREEN non-unity/no-PCH build | `CE3A_GREEN_BUILD2.log`: Succeeded (первый прогон `CE3A_GREEN_BUILD.log` упал на deprecated `GLevelEditorModeToolsIsValid` и отсутствующем `Selection.h`) |
| `Mimir.V5.Composite.EditMode` | `CE3A_GREEN_TEST2.log`: 16/0; после flake-fix `CE3A_GREEN_TEST3_{1,2,3}.log`: 16/0 ×3 |
| полный NullRHI suite | `CE3A_FULL.log`: 247/1 (flake CE-2b, см. §2) → `CE3A_FULL2.log`: `Success=248 Fail=0 (245 + 3)` |
| force-unity | `CE3A_FORCE_UNITY2.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE3A_STRICT2.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Editing/MHCompositeEditorMode.{h,cpp}` (новые; toolkit
и команды внутри), `Private/Composite/MHCompositeLevelSubsystem.cpp`,
`Private/MimirCompositeEditorModule.cpp`, `MimirCompositeEditor.Build.cs`.
Tests: `MHCompositeEditModeTest.cpp` (новый), `MHCompositeEditProjectionTest.cpp` (flake-fix). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- PIE при «Stay»: v1 не блокирует старт PIE (CE-0 spike не нашёл публичного
  отменяющего хука в `PreBeginPIE`); проекция editor-only, в PIE не попадает.
  Полевая проверка в CE-3b/CE-6.
- Следующий — CE-3b: клик по строке Composite Outliner и по геометрии
  проекции выделяет компонент узла (gizmo на узле), editing-tint проекции,
  вход во вложенное определение из режима (breadcrumbs), keys-роутинг под
  флагом.
