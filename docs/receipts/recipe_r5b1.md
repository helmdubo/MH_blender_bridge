# R5b-1 (Recipe Model v2.1) — размещения материализуют static-листья через пул

Статус: **MERGED** (близнец, #116 `1eda77d`). Вторая половина R5 по KICKOFF §5 / docs/16 §2.8:
компилятор размещений, актор, маршрутизация reimport-уведомлений и Outliner
переведены на `UMHInstancePoolSubsystem` (R5a) и его reconcile (R5b-0).
Закрывает OPEN-R-1 (Undo для пула).

## 1. Что сделано

- **Строки материализации** (`FMHCompositeLeafMaterialization`): поле
  `FMHInstanceHandle Handle` — стабильная идентичность пулового листа;
  `Component/InstanceIndex` — текущий ISM-адрес за хэндлом, актор освежает его
  из пула при каждом чтении (`GetLeafMaterializations`,
  `FindLeafMaterialization`, `FindLeafMaterializationByNodePath`).
  `IsInstanced()` = хэндл выставлен либо индекс задан (скрытый пуловый лист
  остаётся instanced с `INDEX_NONE`).
- **Компилятор** (`MHCompositePlacementCompiler.cpp`): полный путь —
  `Pool->RemoveOwner(actor)` и `Pool->Add(actor, NodePath, level,
  FromMesh(mesh, layout, base), world, channels)` для каждого static-листа в
  одном bulk-скоупе; на акторе ISM больше нет (ключ бакета компилятора,
  `PlanViewConfigureBucket`, теги `MH.ISMBucket:N`,
  `MHMigrateCompositePlacementBucket` удалены — бакет описывает
  `FMHPoolBucketDescriptor::FromMesh`). Без пула (не-editor мир) static-листья
  идут прежним путём обычных `UStaticMeshComponent`. Reseed-diff: тот же
  позиционный/по-пути матчинг, применяется через хэндлы (`Remove/Update/
  UpdateAppearance/Add`), без арифметики ISM-индексов и без владения бакетом;
  проверка «endpoint за хэндлом = меш бакета» сохранена. Basis-update и
  appearance fast-path — через `Update/UpdateAppearance` по хэндлам, desync-коды
  прежние (`MH_E_PLACEMENT_STATE_DESYNC`).
- **Актор**: `ClearDerivedComponents` → `RemoveOwner` (Destroyed, Undo,
  смена ассета); `PostEditUndo` без изменений = `RemoveOwner` + rebuild из
  записи актора — **OPEN-R-1 закрыт**; `SetIsTemporarilyHiddenInEditor` и
  `PostEditChangeProperty` синхронизируют видимость пула
  (`SetOwnerEditorVisibility(!IsHiddenEd())`), то же после каждой успешной
  материализации; `ReconcileEndpoint` больше не мигрирует бакеты сам —
  принимает уже мигрированные пулом компоненты за хэндлами (обновляет строки и
  `LeafPlacementComponents`, `++PreviewRevision` + broadcast), а свои
  непуловые `UStaticMeshComponent` (лист, извлечённый для Edit Mode) обновляет
  как раньше.
- **Уведомления** (`MHCompositePlacementEvents.cpp`): дельта интерфейса меша
  применяется к пулу каждого затронутого мира **один раз на бакет**
  (`ReconcileMesh`) до обхода акторов; первая admission по-прежнему rebuild.
- **Outliner**: клик по строке резолвит текущий ISM-адрес через
  `FindLeafMaterializationByNodePath`; выбор инстанса во вьюпорте принимается,
  если `FindLeafMaterialization(component, index)` (через `ReverseLookup` пула)
  называет текущий актор — проверка «owner компонента == актор» снята.

## 2. Тесты (red `29c3a76`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Pool.PlacementMaterializesThroughPool` | два размещения одного меша делят один бакет на pool-акторе (4 и 2 инстанса), актор не владеет ISM, каждая строка: хэндл валиден, бакет пула, transform = план × basis, `ReverseLookup`/`FindLeafMaterialization` называют актор и путь; move A двигает только A в тех же бакетах; hide A → 0 живых у A, B рендерится; show → 3; reseed A без нового бакета; destroy A освобождает только A |
| `Mimir.V5.Composite.Pool.UndoRestoresPooledPlacement` | транзакция move → undo: позиция актора и все инстансы восстановлены, без дублей, соседнее размещение нетронуто (OPEN-R-1) |
| `Mimir.V5.Composite.Pool.ReimportMigratesSharedBucketOnce` | дельта дескриптора меша: `BucketsMigrated == 1` на два размещения, ни один актор не rebuild'ится, старый компонент уничтожен, строки обоих акторов указывают на новый бакет, меш B нетронут |

Переписанные замороженные ожидания (KICKOFF §7.5, по контракту пула):
`ISMMaterializationTest` (бакеты ищутся по строкам, а не по derived-компонентам
актора; инвариант «дрейф политики → новый компонент» сохранён и теперь
исполняется пулом, R5b-1a), `ResourceReconcileTest` (`Bucket()` по строкам),
`PlacementLifecycleTest` (`LifecycleLeafCount` = живые инстансы пула + свои
SMC), `PreviewDefectsTest`, `AsyncEndpointTest` (компонент листа по строке),
`CompositeV5Test` (общий бакет принадлежит pool-актору, не «absolute под
корнем размещения»), `AppliedPlanAdmissionTest` (derived = хэндлы, лист в
пуле, «лист сохраняет объект» — по строке), `CompositePlacementTest`
(восстановленный endpoint — пуловый инстанс, без placeholder'а на акторе),
`BreakTest`
(ISM мира считаются до Break, бакеты пула переживают размещение),
`ISMCottageMetricsTest` (метрики считают пул).

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`29c3a76`) | `R5B1_RED_TEST.log`: три новых теста Fail (нет бакетов пула, ISM на акторе) |
| GREEN non-unity/no-PCH build | `R5B1_GREEN_BUILD6.log`: Succeeded |
| `Mimir.V5.Composite.Pool` | `R5B1_GREEN_TEST.log`: 6/0 |
| полный NullRHI suite | `R5B1_GREEN_FULL4.log`: `Success=212 Fail=0` (209 + 3); первые прогоны: `R5B1_GREEN_FULL.log` — краш `BodySetup` на дрейфе бакета → R5b-1a; `FULL2/3` — два переписанных ожидания |
| `BuildPlugin -StrictIncludes` | `R5B1_STRICT.log`: ExitCode=0 (Success) |
| force-unity | `R5B1_FORCE_UNITY.log`: Succeeded |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHCompositePlacementCompiler.{h,cpp}`,
`Public|Private/Composite/MHCompositeActor.{h,cpp}`,
`Private/Composite/MHCompositePlacementEvents.cpp`,
`Private/UI/MHCompositeOutliner.cpp` (6). Tests:
`MHCompositePoolMaterializationTest.cpp` (новый) и переписанные ожидания
(§2). Docs: `docs/16_recipe_model.md` §2.8/§9 (OPEN-R-1),
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы и следующий срез

Открытых нет. **R5b-2** (близнец): клик по инстансу пула во вьюпорте выделяет
pool-актор движка, а не композит — нужен selection-seam (перенаправление
выделения на owner через `ReverseLookup`, hit proxy), плюс валидация дрейфа
живого компонента бакета относительно дескриптора пула. Полевой тест owner:
Outliner-выбор по клику в сцене, скрытие актора (H), Undo/Redo, reimport меша
с изменением слотов материалов при двух размещениях.
