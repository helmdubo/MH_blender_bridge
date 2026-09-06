# CE-3b (Composite Edit Mode) — выделение узлов через проекцию, editing-tint, клавиши под режимом

Статус: **REVIEW** (близнец). Вторая половина карточки CE-3 спецификации
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md`, CE-ADR-2);
вход во вложенное определение из открытой сессии (breadcrumbs) — CE-3c.

## 1. Что сделано

- Выделение узла (`UMHCompositeEditorMode::SelectComponent`): projection-актор
  выделяется эксклюзивно, затем компонент узла — стандартный gizmo движка
  сидит на узле. Чужие компоненты отклоняются.
- Клик во viewport (`ILegacyEdModeViewportInterface::HandleClick` →
  `HandleHitProxy`): `FEditorModeTools::HandleClick` вызывается раньше
  собственной логики viewport'а, поэтому режим владеет кликом целиком:
  `HActor` на projection-акторе → узел под курсором (без двойного клика,
  который требует движок для компонентов); `HActor` на любом другом акторе и
  `HInstancedStaticMeshInstance` (бакеты пула) — проглатываются (locked
  context); оси gizmo и пустое место — не наши (`false`).
- `UMHCompositeEditProjection::FindComponentForOrigin(origin)` — компонент
  по пути узла (тот же ключ, что `NodePath` строки Composite Outliner).
- Composite Outliner под флагом: клик по строке → `Mode->SelectComponent`
  (строки, которых проекция не несёт, выделение не трогают); `FocusEditScope`
  после Edit Contents хватает первый узел draft'а; подсказка в статусе —
  «Save / Cancel are in the viewport, Esc cancels».
- Editing-tint: `UMHCompositeEditProjection::PushEditingTint` —
  `PushLevelInstanceEditingStateToProxy(true)` на каждый примитив проекции
  (публичный путь движка для акторов редактируемого Level Instance; флаг
  актора `ELevelInstanceFlags::IsInEditHierarchy` закрыт: конструктор
  `FAddActorLevelInstanceFlags` private для level-streaming классов — первый
  green-прогон упёрся в C2248). Идемпотентно по scene proxy: вызывается после
  `Refresh` и из `ModeTick` режима, пересозданный proxy (меш/материал/
  видимость) помечается снова. Под show flag `EditingLevelInstance` (CE-3a)
  всё, кроме проекции, приглушено.
- Клавиши (`MHEditSessionKeys`, только .cpp): под режимом Esc →
  `RequestCancel` (вопрос при dirty; из input-path — отложенно на следующий
  тик с epoch-guard, как Apply в CE-pre), Enter — ничего (не перехватывается,
  Save только кнопкой). Legacy-путь без флага не изменён.

## 2. Тесты (red `9b61c45`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Selection.ClicksGrabTheProjectionNode` | `FindComponentForOrigin` для листа и сгруппированного листа, null для чужого origin; `SelectComponent` → projection-актор и компонент выделены, чужой компонент отклонён; `HandleHitProxy(HActor(projection, grouped))` → grouped выделен, plain снят; `HActor(B)` проглочен и B не выделен; `nullptr` → `false` |
| `…Selection.ProjectionCarriesTheEditingTint` | у проекции есть примитивы, `PushEditingTint` безопасен и идемпотентен без proxy; **RHI lane** (`-RenderOffscreen -MHPreviewRenderSmoke`, как `Mimir.Audit.MainBaseline.RenderedNativeHitProxy`): lane подменяет меш примитивов проекции и чужого бакета на `/Engine/BasicShapes/Cube` (у fixture-мешей нет render data → `CreateSceneProxy` даёт null даже с RHI; пересозданный proxy — ровно то, что должен покрыть push из `ModeTick`), после `SendAllEndOfFrameUpdates` + `PushEditingTint` + `FlushRenderingCommands` scene proxy каждого примитива проекции `IsEditingLevelInstanceChild()`, чужой бакет — нет. Под `-nullrhi` scene proxy не создаются (второй green-прогон `CE3B_GREEN_TEST2.log`: 18/1 на «scene proxy not null»), поэтому lane отделён. В red-коммите тест смотрел на флаг актора (недоступен, см. §1) |
| `…Selection.KeysFollowTheMode` | dirty-сессия: Enter → `false`, без подтверждения перезаписи, сессия жива; Esc → вопрос (Stay: сессия и режим живы), Esc → Discard: ушли |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`9b61c45`) | `CE3B_RED_TEST.log`: Fail ×3 (`CE3B_RED_TEST2.log`; первый red-билд упал на `struct HHitProxy`, поправлено в `9b61c45`) |
| GREEN non-unity/no-PCH build | `CE3B_GREEN_BUILD4.log`: Succeeded (прогоны 1-3: C2248 на private `FAddActorLevelInstanceFlags`, затем переход tint-теста на proxy и RHI lane) |
| `Mimir.V5.Composite.EditMode` | `CE3B_GREEN_TEST4.log`: 19/0 |
| RHI lane `…Selection.ProjectionCarriesTheEditingTint` | `CE3B_RHI_TINT.log`: `CE3B_RHI_TINT2.log`: 1/0 (первый прогон `CE3B_RHI_TINT.log` — null proxy у fixture-мешей без render data → подмена на Cube) |
| полный NullRHI suite | `CE3B_FULL.log`: `Success=251 Fail=0 (248 + 3)` |
| force-unity | `CE3B_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE3B_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (6): `Public|Private/Editing/MHCompositeEditorMode.{h,cpp}`,
`Public|Private/Editing/MHCompositeEditProjection.{h,cpp}`,
`Private/UI/MHCompositeOutliner.cpp`, `Private/UI/MHEditSessionKeys.cpp`.
Tests: `MHCompositeEditModeSelectionTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Визуальная проверка owner'ом с включённым `bCompositeEditModeV2`:
  dimming/tint, gizmo на узле после клика по строке и по геометрии, Esc с
  вопросом. Перемещение gizmo пока обновляет только компонент проекции, не
  draft — CE-4a (event-driven transforms через `InputDelta/EndTracking`).
- CE-3c: Edit Contents на более глубоком субкомпозите из открытой сессии
  (сейчас `BeginEditNestedComposite` отвечает «another session is active»),
  навигация по breadcrumb.
