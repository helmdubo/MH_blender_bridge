# R5b-2b (Recipe Model v2.1) — selection-seam вьюпорта: клик по инстансу пула выделяет owner-композит

Статус: **REVIEW** (близнец). Вторая половина R5b-2 (первая — R5b-2a,
подсветка и bounds). Закрывает полевой дефект owner 2026-09-05: «клик по
инстансу выделяет служебный pool-актор».

## 1. Что сделано

- **Адаптер** `Private/Composite/MHCompositeSelectionAdapter.cpp`
  (`MHRegisterPoolInstanceSelection`, `MHIsPoolInstanceSelectionRegistered`):
  customization typed elements для `NAME_SMInstance` на element-selection-set
  редактора. Для инстанса пулового бакета `GetSelectionElement` идёт через
  `UMHInstancePoolSubsystem::ReverseLookup` → owner-композит (его actor
  element, далее штатная actor-customization), а на акторе фиксируется, какой
  лист задет (`SelectPlacementLeafByNodePath`). Pool-актор никогда не
  становится элементом выбора; устаревший индекс → ничего. Для чужих ISM
  повторена штатная логика уровня (инстанс → компонент → актор при одиночном
  клике; инстанс при повторном), потому что у движка одна customization на тип
  элемента и наша её замещает. `CanSelect/CanDeselect` — как у движка
  (заблокированный уровень, `GEdSelectionLock`).
- **Регистрация** (`MimirCompositeEditorModule`): на
  `FLevelEditorModule::OnLevelEditorCreated` и сразу при старте, если уровень
  редактора уже создан — `SLevelEditor::Initialize` регистрирует свою
  customization раньше этого события, наша ложится поверх. Не зависит от
  Composite Outliner.
- **Outliner**: при выделении композита раскрывает строку листа, записанного
  адаптером (`GetSelectedPlacementLeafPath`).

## 2. Тесты (red `ee9c534`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Selection.PoolInstanceResolvesToOwnerActor` | на собственном `UTypedElementSelectionSet`: регистрация идемпотентна; инстансы A и B на одном бакете резолвятся в A и B соответственно (Primary и Secondary), актор запоминает задетый лист; pool-актор — не элемент выбора; устаревший индекс → пусто; чужой ISM → его компонент/актор, никогда композит; интеграция: на element-selection-set редактора адаптер зарегистрирован модулем и резолвит инстанс в owner |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`ee9c534`) | `R5B2B_RED_TEST.log`: Fail (ничего не регистрируется, инстанс → pool-актор) |
| GREEN non-unity/no-PCH build | `R5B2B_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.Selection` | `R5B2B_GREEN_TEST.log`: 1/0 (интеграционная ветка на selection-set редактора выполнена) |
| полный NullRHI suite | `R5B2B_GREEN_FULL.log`: `Success=219 Fail=0` (218 + 1) |
| force-unity | `R5B2B_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R5B2B_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositeSelectionAdapter.{h,cpp}` (новые),
`Public|Private/MimirCompositeEditorModule.{h,cpp}`,
`Private/UI/MHCompositeOutliner.cpp`. Tests:
`MHCompositeSelectionAdapterTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы и полевой протокол owner

Открытых нет. Полевой тест (портфолио): клик по инстансу в сцене при закрытом
Composite Outliner выделяет композит, не pool-актор; подсвечены только его
инстансы; `F` кадрирует размещение; Ctrl/Shift-клик по инстансам двух
композитов на одном меше выделяет оба композита; Delete/Duplicate/gizmo
действуют на композит; клик по обычному ISM/foliage ведёт себя как раньше;
Game View (G) показывает инстансы.
