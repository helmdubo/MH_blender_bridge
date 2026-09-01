# Квитанция U5 — ISM-материализация статических листьев

Статус: **READY FOR OWNER FIELD TEST**. Код, Automation и build-гейты
зелёные; остаётся только визуально проверить native instance hit во viewport
на рабочей сцене.

## 1. База и границы

- ветка: `codex/u5-ism-materialization`;
- актуальная база: `origin/main` `b5a694f` (проверено повторным `git fetch`);
- stock Unreal Engine 5.7.4; Engine не менялся и fork Engine не создавался;
- изменения ограничены `ue/MimirComposite` и этой квитанцией;
- runtime/cook-материализатор, tiny transport, формат, resolver, подписи,
  `golden/` и `reference/` не изменялись;
- новая authority, enum и E/W-коды не вводились.

Изолированный host:
`E:\MimirComposite_U5_20260901\MimirCompositeV5S6.uproject`.
Все editor-тесты и сборки выполнялись без перехвата desktop UI.

## 2. Red-first

На исходной по-листовой материализации сначала был добавлен тест
`Mimir.V5.Composite.ISM.BucketsIdenticalStaticLeaves` с 12 одинаковыми
static-mesh листьями.

Красный результат:

```text
Result={Fail} Name={BucketsIdenticalStaticLeaves}
Expected 'identical static leaves share one ISM bucket' to be 1, but it was 0.
```

Лог:
`E:\MimirComposite_GameObj_Audit_20260828\Saved\Logs\MimirCompositeV5S6-backup-2026.08.31-21.04.21.log`,
строки 3369–3379.

После реализации тот же тест зелёный: один ISM bucket, 12 instances,
12 plan-aligned mapping rows; instance transforms, `ResolvedNodeIndex`,
`NodePath`, Outliner mapping и Edit Mode extraction проверены.

Дополнительный red hardening дал guarded no-PCH lane: скрытые зависимости
`TGreater` и `TObjectPtr` не компилировались без PCH. Добавлены только прямые
include `Templates/Greater.h` и `UObject/ObjectPtr.h`; финальный тот же lane
зелёный.

## 3. Реализация

### 3.1 Полный ключ бакета

`FPlanViewISMBucketKey` включает без пропусков:

- `UStaticMesh`;
- упорядоченный `OverrideMaterials`;
- collision profile, enabled mode, object type, response container,
  overlap events, complex-on-move и physical-material return;
- component `CastShadow`, `AffectDistanceFieldLighting`,
  `VisibleInRayTracing`;
- секционные material/collision/shadow/ray-tracing/distance-field/
  force-opaque флаги;
- mobility;
- editor visibility и hidden-in-game;
- полный CPD layout (`BaseIndex + MH_APPEARANCE_CHANNELS`), а не только
  число добавленных каналов.

Любое несовпадение запрещает reuse бакета. Тест
`BucketPolicyRejectsMutatedReuse` по очереди мутирует каждую группу политики
и требует новый bucket с сохранением всех instances.

### 3.2 Материализация

- static leaves группируются в `UInstancedStaticMeshComponent` внутри одного
  `AMHCompositeActor`;
- в instance передаётся матрица `Leaf.WorldMatrix * ActorBasis` как полный
  matrix product; композиция авторских `FTransform` не вводилась;
- те же четыре appearance-канала пишутся в `PerInstanceCustomData`;
- `LeafPlacementComponents` остаётся plan-aligned compatibility view и может
  содержать повторяющийся указатель на bucket;
- точная authority для навигации — `FMHCompositeLeafMaterialization`:
  `(Component, InstanceIndex, ResolvedNodeIndex, NodePath)`;
- actor/gameobj/diagnostic/companion компоненты остаются обычными и проходят
  прежним путём.

Для synthetic `UStaticMesh` без `BodySetup` применяется `NoCollision`: это
fail-closed эквивалент фактического поведения прежнего SMC и не ослабляет
политику реального managed mesh.

### 3.3 Selection, Outliner и Edit Mode

- ISM создаётся с per-instance hit proxies;
- Outliner model индексирует `(ISM, InstanceIndex)` и находит точный
  `NodePath`/resolved node;
- row → viewport использует штатный editor SM-instance element handle;
- viewport typed-selection → row использует тот же plan-aligned mapping,
  без custom hit proxy;
- выбранный leaf при входе в Placement Edit Mode выносится в один обычный
  `UStaticMeshComponent`; остальные instances остаются в бакетах;
- при выходе leaf возвращается в bucket;
- pending handle transforms перед rebuild Edit Mode сохраняются и
  восстанавливаются до подписи prospective plan.

### 3.4 Lifecycle и инкрементальный reseed

Первичная materialization по-прежнему вызывается только из существующей
`PostRegisterAllComponents`-цепочки; `OnConstruction` остаётся basis-update.
Orphan-union и appearance fast-path сохранены и стали ISM-aware.

Seed-only reconcile проверяет точное соответствие rows/buckets/instance
indices и ключа. Изменённые instances удаляются в убывающем порядке индексов,
стабильные получают только нужный transform/CPD update. При любой структурной
нестыковке используется прежний полный fail-closed rebuild.

Путь generated meshes не хардкодился заново. Более того, старый literal из
reseed validation удалён: identity endpoint проверяется существующим loader/
definition entry.

## 4. Реальный cottage: до/после

Фикстура: установленный `sovmod_cottage_i_cmp`, фиксированные
`LayoutSeed=1729`, `AppearanceSeed=2718`, два последовательных размещения;
ниже warm-второе размещение на одном host и DDC.

| Метрика | До U5 | После U5 | После/до |
|---|---:|---:|---:|
| resolved leaves | 849 | 849 | 100% |
| derived/scene components | 850 | 336 | 39.53% |
| static materializers | 849 SMC | 335 ISM | 39.46% |
| ISM instances | 0 | 849 | — |
| полный warm wall | 140.5263 ms | 104.3183 ms | 74.23% |
| CompilePlacement | 52.5179 ms | 18.9702 ms | 36.12% |
| RegisterComponents | 20.3299 ms / 850 calls | 0.9114 ms / 336 calls | 4.48% / 39.53% |

Итого: компонентов меньше на **60.47%**, materializers меньше на **60.54%**,
warm placement быстрее на **25.77%**, CompilePlacement быстрее на **63.88%**,
время регистрации меньше на **95.52%**. У cottage 335 разных полных ключей,
поэтому 849 листьев честно не могут стать одним bucket.

Cold wall был 150636.0703 ms до и 148170.8277 ms после. В обоих случаях
доминируют первое построение graph/static-mesh render data/DDC
(128–130 s BuildAppliedGraph и 19–20 s endpoint/build work); это не цена
материализации и не предмет U5.

Отчёты:

- до: `E:\MimirComposite_U5_20260901\Reports\CottageBaselineFixed\index.json`;
- после: `E:\MimirComposite_U5_20260901\Reports\CottageU5Fixed\index.json`.

## 5. Automation и сборки

| Гейт | Результат |
|---|---|
| `Mimir.V5.Composite.ISM` | **3/3**, 0 failed; два synthetic acceptance + real-asset probe |
| `Mimir.V5.Composite.Lifecycle` | **7/7**, 0 failed |
| `Mimir.V5.Composite.DefinitionPool` | **9/9**, 0 failed |
| `Mimir.V5.Composite.IncrementalReseed` | **6/6**, 0 failed |
| полный NullRHI `Mimir` с `-MHGoldenRoot` и parity-host | **168/168**, 0 failed, 0 not run: 124 clean + 44 expected-warning |
| guarded force-unity: `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors` | **Succeeded**, финальные 10/10 actions |
| `BuildPlugin -StrictIncludes` (`-DisableUnity -NoPCH -NoSharedPCH`) | **BUILD SUCCESSFUL**; Editor 111/111, Runtime Development 11/11, Runtime Shipping 11/11 |
| `git diff --check` | **PASS** |

Финальный полный отчёт:
`E:\MimirComposite_U5_20260901\Reports\U5FullMimirFinal2\index.json`.

Важно: cottage probe в полном synthetic host корректно пишет informational
`NOT RUN`, потому что portfolio asset там не установлен; отдельный реальный
lane выше выполнил тот же тест и дал числа до/после.

## 6. Изменённые файлы

Production:

- `MimirCompositeEditor.Build.cs`;
- `MHCompositeActor.{h,cpp}`;
- `MHCompositePlacementCompiler.{h,cpp}`;
- `MHCompositeOutliner.{cpp}`;
- `MHCompositeOutlinerModel.{h,cpp}`.

Tests/metrics:

- `MHCompositeISMMaterializationTest.cpp`;
- `MHCompositeISMCottageMetricsTest.cpp`;
- адаптация существующих admission/definition/reseed/lifecycle/preview/seed/
  V5/parity тестов к plan-aligned ISM mapping.

`golden/`, `reference/`, runtime module, importer и diagnostic registries в
diff отсутствуют.

## 7. Полевой протокол owner

1. Разместить `sovmod_cottage_i_cmp`, убедиться, что в actor около 335 ISM
   buckets/849 instances и нет 849 обычных leaf SMC.
2. Кликнуть отдельный instance во viewport: Outliner должен подсветить точный
   `NodePath`.
3. Кликнуть другую leaf-row в MH Composite Outliner: viewport selection должен
   перейти на точный `(bucket, instance index)`.
4. Для выбранного leaf включить Placement Edit Mode: появляется один обычный
   SMC, остальные листья остаются instanced; после выхода снова только buckets.
5. Выполнить targeted Reimport одного используемого mesh: placement должен
   обновиться существующей notify-воронкой, selection actor и mapping не должны
   ломаться.

## 8. Вопросы и fail-closed допущения

Открытых архитектурных вопросов нет.

- Контекст: один bucket может содержать листья разных top-level handles.
  Вопрос: физическая attachment-иерархия на instance невозможна.
  Временное fail-closed допущение: source hierarchy хранится только в
  immutable plan и явном `(bucket,index) -> NodePath`; bucket имеет absolute
  identity basis под root актора.
- Контекст: вход в Edit Mode без выбранного валидного leaf.
  Вопрос: какой leaf выносить автоматически, не ратифицировано.
  Временное fail-closed допущение: ничего не выбирать и не извлекать; обычный
  SMC появляется только для явно выбранного mapping row.
