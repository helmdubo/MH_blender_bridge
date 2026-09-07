# CE-6b1 (Composite Edit Mode) — режим включён по умолчанию

Статус: **REVIEW** (близнец). Первая половина cutover'а карточки CE-6
(«Новый mode — единственный production Edit entry»), по прямому решению
owner 2026-09-07: включить режим сразу, не дожидаясь отдельной приёмки.
Снятие legacy-кода (хэндлы, `Tick`, `SetPlacementEditMode`,
`EditingDocument`/`EditingGraph` актора) — CE-6b2, после полевой проверки.

## 1. Что сделано

- `UMHCompositeSettings::bCompositeEditModeV2` = **true** по умолчанию:
  проект без правок конфигурации получает Composite Edit Mode (сессия +
  draft + projection-актор + режим с панелью `Save | Cancel`) и для Edit
  размещения, и для Edit Contents подкомпозита. Выключение флага
  возвращает legacy-путь (оставлен до CE-6b2).
- Свойство получило понятный заголовок в настройках: **Project Settings →
  Plugins → Mimir Composite → Mimir Composite | Edit → «Composite Edit Mode
  (session + projection)»** (`config = Editor`, `defaultconfig`).
- Тесты, описывающие legacy-путь, теперь явно фиксируют backend:
  общий `FMHCompositeEditBackendScope(bool)` в `MHRecipeTestFixture.h`
  (одно определение в заголовке — не сталкивается в unity-blob'ах),
  применён в `EditContext` (11 тестов), `EditMode.Characterization` (4),
  `EditMode.Session` (2), `EditMode.Mode.LegacyBackendNeverActivatesIt`,
  `LevelOperations`. Тесты нового backend'а свои per-file scope'ы
  сохранили — они теперь просто совпадают с умолчанием.

## 2. Тесты (red `96f354a`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Cutover.TheModeIsTheProductionBackendByDefault` | без единого scope: настройка включена; `BeginEditNestedComposite` → режим активен, размещение не в legacy edit-mode, проекция открыта; `BeginEditComposite` → то же для корня; с `FMHCompositeEditBackendScope(false)` → режим не активируется, размещение держит хэндлы (legacy достижим) |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`96f354a`) | `CE6B1_RED_TEST.log`: 49/1 — упал только новый cutover-тест |
| GREEN non-unity/no-PCH build | `CE6B1_GREEN_BUILD.log`: Succeeded |
| `EditMode`+`EditContext`+`LevelOperations` | `CE6B1_GREEN_TEST.log`: 50/0 |
| полный NullRHI suite | `CE6B1_FULL.log`: `Success=270 Fail=0 (269 + 1)` |
| force-unity | `CE6B1_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE6B1_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (1): `Public/Settings/MHCompositeSettings.h`. Tests:
`MHRecipeTestFixture.h` (общий scope), `MHCompositeEditCutoverTest.cpp`
(новый), пять файлов legacy-тестов (по одной строке scope). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Проект owner'а (`MimirHead_portfolio 5.7/Config/DefaultEditor.ini`)
  флаг не пинует — новое умолчание действует сразу после установки
  плагина; если кто-то раньше сохранял настройку, в ini появится строка
  `bCompositeEditModeV2=False`, и её надо убрать.
- CE-6b2 (после полевой проверки owner'а): снятие legacy-пути и перенос
  оставшихся legacy-тестов на сессию; тогда же уйдёт и сам флаг.
