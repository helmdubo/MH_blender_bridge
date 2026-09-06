# CE-3c (Composite Edit Mode) — корневое определение под CE-backend

Статус: **REVIEW** (близнец). Закрывает дыру CE-2b/CE-3: под флагом
`bCompositeEditModeV2` вложенные определения шли через сессию + проекцию, а
Edit корневого композита — по-прежнему через legacy-путь (хэндлы в размещении,
`Tick`). Теперь под флагом один backend для обоих случаев; legacy остаётся
только при выключенном флаге (до CE-6). Переход между определениями из
открытой сессии и breadcrumb-навигация — CE-3d.

## 1. Что сделано

- `BeginEditComposite` под флагом: та же сессия CE-1 (`InvocationPath` пустой,
  `IsNested() == false`), draft, проекция и режим CE-3a; размещение **не**
  входит в legacy edit-mode (`SetPlacementEditMode` не вызывается, top-level
  компоненты не захватываются). Ошибка проекции откатывает сессию.
- `CommitEditComposite` под сессией берёт документ из draft'а
  (`Extract`), без `Tick(0)` и `GetEditedCompositeDocument`; публикация,
  reconcile-fallback и `ResetTransaction` — без изменений. `CancelEditComposite`
  для корневой сессии не перестраивает размещение: снятие lease возвращает
  инстансы.
- Проекция (`UMHCompositeEditProjection::Open`) принимает корневую сессию:
  occurrence = всё размещение (`OccurrencePrefix` пустой → все листья и все
  строки пула в lease), `DefinitionPrefix = <root>:`. Пустой префикс явный
  (`UnderOccurrence`): `FString::StartsWith("")` в UE возвращает false —
  первый green-прогон дал проекцию без листьев и пустой lease
  (`CE3C_GREEN_TEST.log`: 20/1), тест тоже ключует «всё размещение» на
  `<root>:`.
- Вложенная ссылка атомарна (spec A04): её геометрия проецируется (SMC) и
  отображается на reference-узел draft'а, но ни один структурный узел ниже
  `>` хэндла не получает. Это чинит и вложенный случай: раньше внуки
  редактируемого определения получали хэндлы.

## 2. Тесты (red `b6dbf46`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Root.SessionProjectsTheWholePlacement` | под флагом `BeginEditComposite(A)`: A не в legacy edit-mode; сессия не nested, редактируется root-ассет, режим активен; собственный лист root — SMC, ссылка `nodes[1]` — хэндл, вложенный лист — SMC и → узел 1 (A04), вложенная группа — без хэндла; SMC на каждый лист размещения в тех же мировых точках, lease ровно на все строки A; B и чужой ISM не тронуты; `SetNodeTransform(+100 X)` двигает проекцию, dirty; Cancel → режим снят, размещение там же, legacy edit-mode так и не включался |
| `…Root.SaveAppliesTheDraft` | правка узла 0 → `RequestSave`: одно подтверждение перезаписи, опубликован именно root, сессия и режим закрыты, legacy edit-mode не включался, root несёт правку |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`b6dbf46`) | `CE3C_RED_TEST.log`: Fail ×2 |
| GREEN non-unity/no-PCH build | `CE3C_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE3C_GREEN_TEST2.log`: 21/0 (первый прогон `CE3C_GREEN_TEST.log`: 20/1 — пустой префикс, см. §1) |
| полный NullRHI suite | `CE3C_FULL.log`: `Success=253 Fail=0 (251 + 2)` |
| force-unity | `CE3C_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE3C_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (2): `Private/Composite/MHCompositeLevelSubsystem.cpp`,
`Private/Editing/MHCompositeEditProjection.cpp`. Tests:
`MHCompositeEditRootSessionTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Edit корня из меню Composite Actions под флагом: ничего не выделяется до
  первого клика (Outliner-путь хватает первый узел через `FocusEditScope`);
  единый «выделить первый узел при входе» — вместе с CE-3d.
- `SaveEditAsUnique` для корневой сессии под флагом идёт по существующей
  session-ветке (`Extract` draft'а); отдельного теста нет — покрыто
  `EditContext.MakeUnique*` на legacy-пути и общей веткой CE-2b.
- CE-3d: Edit Contents на другом определении из открытой сессии (одна
  writable-сессия: Save/Discard/Cancel текущей, затем вход), breadcrumb-
  сегменты как кнопки навигации.
