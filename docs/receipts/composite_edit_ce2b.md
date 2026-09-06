# CE-2b (Composite Edit Mode) — проекция выбранного occurrence и флаг backend'а

Статус: **REVIEW** (близнец). Вторая половина карточки CE-2 спецификации
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md` §5.4–5.5,
§6.1, CE-ADR-4).

## 1. Что сделано

- `UMHCompositeSettings::bCompositeEditModeV2` (config, по умолчанию off):
  выбирает backend вложенного Edit Contents целиком. Off — прежний путь
  (actor-хэндлы, `Tick`); on — сессия CE-1 + проекция, корневой актор не
  входит в legacy edit-mode. Два писателя одного draft'а исключены.
- `AMHCompositeEditProjectionActor` (transient, editor-only, hidden in game,
  не в World Outliner; spawn как у `ALevelInstanceEditorInstanceActor`:
  `OverrideLevel` = уровень root, `bCreateActorPackage=false`, `RF_Transient`)
  — решение owner: компоненты одного актора, не actor на узел.
- `UMHCompositeEditProjection` (outer = сессия): `Open` → spawn актора,
  `Refresh`, затем lease на строки root под `<invocation>>` (сначала
  проекция, потом подавление — оригинал не исчезает раньше замены);
  `Refresh` — граф размещения (`Compile(root)` + `MHBuildRecipeGraph`) с
  заменой редактируемого определения на draft, скомпилированный тем же
  компилятором через transient `UMHCompositeAsset` (никакого второго
  document→graph пути), `MHResolvePreviewGraph` под замороженными
  seed/appearance seed/`CallContext` сессии; листья под occurrence →
  `UStaticMeshComponent` (тот же меш через `ResolveMeshForPreview`, те же
  appearance-каналы через `MHApplyLeafAppearanceCustomData`, без коллизии и
  навигации), структурные узлы (group/random/nested reference/actor) →
  `USceneComponent`-хэндл; компоненты живут по origin (тег
  `MHEdit.Origin:<path>`), стареющие уничтожаются; `Close` — release lease,
  destroy актора (повторно безопасно).
- Маппинг компонент ↔ узел draft'а: `GetOriginForComponent`,
  `GetNodeIdForComponent` (лист → его узел; лист под `/options[k]` → random-узел;
  всё под `>` → узел-ссылка), `FindComponentForNodeId`.
- `UMHCompositeEditSession`: `OpenProjection/CloseProjection/GetProjection`;
  `SetNodeTransform` после правки draft'а обновляет проекцию; `Close`
  закрывает проекцию.
- Subsystem: `BeginEditNestedComposite` под флагом открывает сессию и
  проекцию (ошибка проекции = отказ Begin, сессия сброшена);
  `CommitNestedEditComposite` и `SaveEditAsUnique` берут документ из draft
  сессии, когда legacy edit-mode не активен.

## 2. Тесты (red `339a9ad`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Projection.ShowsTheOccurrenceAndSuppressesItsInstances` | под флагом: сессия + открытая проекция; root не в legacy edit-mode, хэндлов нет; актор transient/editor-only/hidden в уровне root; по mesh-компоненту на каждый пуловый лист occurrence в тех же мировых точках; инстансы occurrence подавлены, lease ровно на них; первый вызов, B и чужой ISM прежние; `SetNodeTransform` plain (+100 X) → компонент сдвинут, первый вызов того же определения **не** сдвинут (локальный preview), сессия dirty; Cancel → проекция закрыта, актор уничтожен, инстансы вернулись на места |
| `…Projection.ComponentsMapToDraftNodes` | plain и grouped — mesh-компоненты, group — хэндл, random представлен ровно одним (хэндл или выбранный лист); маппинг в id узлов 0/1/2/3; обратный поиск; чужой компонент → пустой id |

Характеризационный тест CE-0 `DraftPreviewFollowsEveryInvocationInThisPlacement`
описывает legacy-путь и остаётся зелёным (флаг off); CE-ADR-4 доказан новым
тестом под флагом.

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`339a9ad`) | `CE2B_RED_TEST.log`: Fail ×2 |
| GREEN non-unity/no-PCH build | `CE2B_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE2B_GREEN_TEST.log`: 13/0 |
| полный NullRHI suite | `CE2B_FULL.log`: `Success=245 Fail=0 (243 + 2)` |
| force-unity | `CE2B_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE2B_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Editing/MHCompositeEditProjection.{h,cpp}` (новые),
`Public|Private/Editing/MHCompositeEditSession.{h,cpp}`,
`Private/Composite/MHCompositeLevelSubsystem.cpp`,
`Public/Settings/MHCompositeSettings.h`. Tests:
`MHCompositeEditProjectionTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Визуальный spike (сохранение карты без актора проекции, PIE, dimming через
`EditingLevelInstance`) — теперь есть что смотреть: включить
`bCompositeEditModeV2` в Project Settings → Mimir Composite → Edit и открыть
Edit Contents; gizmo и выделение компонентов проекции приходят с CE-3.
Корневые сессии остаются на legacy до CE-3. Следующий — CE-3: `UEdMode`,
панель `<breadcrumb> | Exit`, точное выделение узлов проекции.
