# R3b — reconcile по дельте интерфейса endpoint'а вместо полного rebuild

Статус: **READY FOR REVIEW → merge by the twin**.

## 1. База и границы

- Ветка исполнителя `codex/recipe-r3b-resource-reconcile`, PR
  [#108](https://github.com/helmdubo/MH_blender_bridge/pull/108).
- Контракт `2f4bda3`, red-коммит `e00d80a`.
- `OPEN-R3B-1` (ForceAndNotify требовал запрещённый rebuild) снят близнецом:
  вариант 1, коммит `fc08ca1` переписал оба ожидания
  `Mimir.V4.StaticMesh.TargetedReimport.ForceAndNotify` на «reconcile без
  rebuild», сохранив проверки импорта, receipt/геометрии, уведомлений,
  build/save и mesh identity.
- Реализация: коммит `c51f0cc` («Implement R3b resource reconciliation and
  record bounded decisions»), поверх согласованного red-коммита `fc08ca1`.
- Верификация выполнена близнецом на своём generic-хосте
  `E:\MimirComposite_R_M0_20260902`, на голове ветки `c51f0cc`, 2026-09-05.
  Portfolio, установленный plugin и Engine близнеца не менялись.
- DECIDED-R3B-1 и DECIDED-R3B-2 (см. §6) приняты близнецом при ревью.

## 2. Acceptance (7 пунктов контракта)

| № | Требование | Результат |
|---|---|---|
| 1 | Non-unity/no-PCH сборка | Succeeded (`R3B_VERIFY_BUILD.log`) |
| 2 | Зелёные `Reconcile.*`, `Recipe.*`, `DefinitionPool.*`, `Perf.*`, `Seed.*`, `Break.*` | Входят в полный suite ниже, отдельных Fail не зафиксировано |
| 3 | Полный NullRHI suite, фильтр `Mimir.` | Success=202, Fail=0 (`R3B_VERIFY_FULL.log`); ожидаемое число 199 + 3 новых Reconcile-теста совпало |
| 4 | `BuildPlugin -StrictIncludes`; force-unity | StrictIncludes: ExitCode=0 (Success), `R3B_STRICT.log`; force-unity: `R3B_FORCE_UNITY.log` — «Succeeded, см. лог»; близнец подтвердит перед merge |
| 5 | `git diff --check`, `check_normative_docs.py` | см. §9 — прогнаны из этого worktree при финализации квитанции |
| 6 | Квитанция с RED/GREEN, гейтами, таблицей «дельта → действие», замером реимпорта 100 placements | Эта квитанция; замер — **NOT RUN**, см. §6 |
| 7 | Полевое подтверждение owner на портфолио после merge | Не выполнялось (owner-gate после merge) |

## 3. Red-first → green

Собственный прогон исполнителя до STOP (зафиксирован в предыдущей версии
этой квитанции, база `2f4bda3`/`e00d80a`): 202 результата, 0 Fail, три
условных **NOT RUN** (два real-RHI preview lane, требующие
`-MHPreviewRenderSmoke` без NullRHI, и `Mimir.V5.Composite.ISM.CottageMetrics`,
для которого на исполнительском хосте отсутствовал cottage fixture) — эти
три lane не заявляются выполненными.

RED на согласованном коммите `fc08ca1` (до реализации): падали три теста
из `MHResourceReconcileTest.cpp` (`Reconcile.SameInterfaceReimportKeepsBuckets`,
`Reconcile.DescriptorChangeMigratesOnlyAffectedBucket`) и переписанный
`Recipe.ActorReimportViaRecipeDependents` — все требовали отсутствия
rebuild/layout при пустой или не-структурной дельте, а код `fc08ca1` ещё
делал полный `RebuildComposite` по любому mesh-уведомлению.
`Reconcile.ChildRecipeReimportKeepsParentRecipe` был зелёным уже на этой
базе. Переписанные ожидания `ForceAndNotify` — зелёные на `fc08ca1` (это и
есть смысл правки близнеца: старое требование rebuild снято).

GREEN на `c51f0cc`: близнец подтверждает полный NullRHI suite — 202/202,
0 Fail (`R3B_VERIFY_FULL.log`), в том числе все перечисленные выше тесты.

## 4. Гейты

| Гейт | Результат | Лог |
|---|---|---|
| Non-unity/no-PCH сборка | Succeeded | `R3B_VERIFY_BUILD.log` |
| Полный NullRHI suite `Mimir.` | Success=202, Fail=0 | `R3B_VERIFY_FULL.log` |
| `BuildPlugin -StrictIncludes` | ExitCode=0 (Success) | `R3B_STRICT.log` |
| Force-unity | Succeeded — см. лог (близнец подтвердит перед merge) | `R3B_FORCE_UNITY.log` |
| `git diff --check` | см. §9 | — |
| `python tools/check_normative_docs.py` | см. §9 | — |

## 5. Реализация

### 5.1 Маршрутизация в `MHNotifyGeneratedResourceChanged` (`MHCompositePlacementEvents.cpp`)

Для каждого затронутого актора (`DependsOnResource(Key)`) вызывается один
из трёх путей вместо безусловного `RebuildComposite`:

| Ключ | Действие |
|---|---|
| `static_mesh` | Собрать список зависимых акторов; если среди них есть выбранный mesh-leaf либо актор без резолвленного плана — `Registry->Invalidate(Key)` + `Registry->Resolve(Key)` + `GetLastInterfaceDelta(Key)`, затем на каждого зависимого актора `Actor->ReconcileEndpoint(Key, Delta)`. Если потребителей с выбранным листом нет — только lazy `Invalidate`, дельта не вычисляется, `Proofs->InvalidateAll()` остаётся консервативным (как раньше) |
| `composite` | `Recipes->InvalidateComposite` как раньше; на каждого зависимого актора — `Actor->ReconcileRecipe(Key)` |
| `placement_profile` | Без изменений: `InvalidateProfile` + rebuild носителей |
| `material`, `texture` | Реестр endpoint'ов инвалидируется, но цикл по акторам не выполняется (ранний `return` перед перебором) — rebuild placement'ов не запускается |

`Proofs->InvalidateAll()` вызывается безусловно для не-mesh ключей; для
mesh-ключа — только если дельта неизвестна (нет выбранного потребителя) или
`Delta.Any()` истинно. Пустая дельта на mesh с известными выбранными
потребителями не трогает proof cache.

### 5.2 Таблица «дельта → действие» (`AMHCompositeActor::ReconcileEndpoint`)

| Дельта | Реализованное действие |
|---|---|
| пустая (`!Any()`) | функция возвращается сразу, ничего не делает |
| `bFirstAdmission` | если у актора нет резолвленного плана либо план уже содержит выбранный mesh-leaf этого ключа — `RebuildComposite()` (перенос текущего пути восстановления после Invalid, допускается контрактом до R4); иначе ничего |
| `bBucketDescriptor` (для бакетов, чей `GetStaticMesh() == Mesh`) | `MHMigrateCompositePlacementBucket` пересоздаёт **только** этот ISM-бакет тем же `PlanViewConfigureBucket`-путём: копирует collision profile name (см. DECIDED-R3B-2), tags, world transform, custom primitive data и все инстансы с их custom data; заменяет объект в `DerivedComponents`, `LeafPlacementComponents`, `LeafMaterializations`; старый бакет уничтожается через `MHRecordPlacementComponentDestroyed()` + `DestroyComponent()` |
| `bMaterialBinding` | `Component->EmptyOverrideMaterials()` (компонент наследует slot/default/overlay bindings меша напрямую, без пересоздания) |
| `bCollisionInterface` | `Component->RecreatePhysicsState()` |
| после любого из вышеперечисленных на этом компоненте | `MarkRenderStateDirty()` + `UpdateBounds()`; счётчик `MHRecordReimportBucket(bMigrated)` |
| `bBounds` | отдельного явного шага нет: `UpdateBounds()` уже выполняется безусловно по компоненту при непустой дельте; отдельного кэша bounds на акторе нет (bounds выводятся из компонентов) |

Ни один из путей не вызывает `RebuildPlacement`, `MHMaterializeLayout`,
resolver или proof: `PlacementRebuildCount` не растёт. Прирост
`bMigrated` учитывается отдельно от rebuild-счётчика — маршрутизатор в §1
сравнивает `GetPlacementRebuildCount()` до/после `Reconcile()` и пишет
`MHRecordReimportActorRebuild` только при реальном росте, иначе
`MHRecordReimportActorReconciled()`.

### 5.3 `AMHCompositeActor::ReconcileRecipe(Key)` — child-рецепт

Перед вызовом `RebuildPlacement(false, /*bRecipeChanged=*/true)` актор
приводит collision profile name бакетов, унаследованных от неизменившихся
листьев, к каноничному дефолтному имени (см. DECIDED-R3B-2), чтобы
reseed-diff не отверг их как «изменившиеся» только из-за побочного эффекта
setters. Новый параметр `bRecipeChanged` в `RebuildPlacement`: слой
resident-plan сравнения используется так же, как при `bLayoutReseed`
(`PreviousPlan = ResidentPlan`, incremental diff-путь компилятора), но
без записи reseed-специфичных perf-счётчиков (`RecordMHPlacementReseedComparison`,
`MHRecordPlacementReseedIncrementalApplied/FullFallback` теперь вызываются
только при `bLayoutReseed`, не при `bRecipeChanged`). Layout
пересчитывается заново; бакеты с неизменившимися листьями (ресурс +
world-матрица + appearance) сохраняют объект компонента через тот же
incremental compile-путь, что и seed-reseed.

### 5.4 Счётчики (`FMHReimportPerfReport` / `MHPerformanceTrace.{h,cpp}`)

Добавлены в конец структуры (M0-совместимо): `RecipesRecompiled`,
`ParentRecipesRecompiled`, `BucketsRefreshed`, `BucketsMigrated`. Строка
`MH_PERF_REIMPORT` расширена четырьмя полями в конце:
`recipes_recompiled`, `parent_recipes_recompiled`, `buckets_refreshed`,
`buckets_migrated`. `RecipesRecompiled`/`ParentRecipesRecompiled`
считаются только при активной трассировке (`MHIsReimportPerfActive()`):
для composite-ключа обходится дерево зависимых `UMHCompositeAsset` через
`Recipes->GetDependents`, ревизия каждого до/после сравнивается с
`Recipes->GetGeneration()`/`RecipeRevision`, без принудительной
компиляции ради замера. `BucketsRefreshed`/`BucketsMigrated` увеличиваются
`MHRecordReimportBucket(bMigrated)` из `ReconcileEndpoint` для каждого
затронутого бакета.

## 6. DECIDED / OPEN — сводка

- **OPEN-R3B-1** (ForceAndNotify требовал запрещённый rebuild) — закрыт
  близнецом, вариант 1: оба ожидания
  `Mimir.V4.StaticMesh.TargetedReimport.ForceAndNotify` переписаны в
  `fc08ca1` на «reconcile без рост `PlacementRebuildCount`», проверки
  импорта/receipt/уведомлений/build-save/mesh identity сохранены.
- **DECIDED-R3B-1** (admission только при наличии выбранного потребителя) —
  принято близнецом при ревью. Реализовано в §1: `bNeedsMeshAdmission`
  собирается по всем зависимым акторам до `Invalidate`/`Resolve`; без
  выбранного mesh-leaf или резолвленного плана дельта не вычисляется,
  остаётся lazy `Invalidate`.
- **DECIDED-R3B-2** (эквивалентный collision profile перед recipe diff) —
  принято близнецом при ревью. Реализовано в `ReconcileRecipe` (канонизация
  имени профиля перед diff) и в `MHMigrateCompositePlacementBucket`
  (восстановление имени после `PlanViewConfigureBucket`).
- Новых OPEN на этапе верификации близнецом не зафиксировано.

Замер реимпорта одного меша на 100 placements (`MH_PERF_REIMPORT`
до/после, `ActorRebuildMsTotal`/`BucketsRefreshed`), заявленный в acceptance
п.6: исполнитель подготовил отдельный бенчмарк вне репозитория
(`E:\MimirComposite_R3b_20260905`, файлы `R3bBenchmark.cpp` /
`R3bEdgeCases.cpp`), но числа не были сняты и зафиксированы до исчерпания
лимита использования исполнителя. **Замер NOT RUN** — остаётся полевым
тестом owner после merge; числа в этой квитанции не изобретаются.

## 7. Изменённые файлы (`git diff fc08ca1...c51f0cc -- ue/MimirComposite/Source`)

| Файл | +/- |
|---|---|
| `MimirCompositeEditor/Private/Composite/MHCompositeActor.cpp` | +97/-9 |
| `MimirCompositeEditor/Private/Composite/MHCompositePlacementCompiler.cpp` | +34 |
| `MimirCompositeEditor/Private/Composite/MHCompositePlacementEvents.cpp` | +95/-12 |
| `MimirCompositeEditor/Private/Performance/MHPerformanceTrace.cpp` | +27/-1 |
| `MimirCompositeEditor/Public/Composite/MHCompositeActor.h` | +7/-1 |
| `MimirCompositeEditor/Public/Composite/MHCompositePlacementCompiler.h` | +4 |
| `MimirCompositeEditor/Public/Performance/MHPerformanceTrace.h` | +8 |

Итого 7 файлов, 251 добавление / 21 удаление. Тесты, resolver,
`MHCompiledRecipe.*`, `MHMaterializeLayout.*`, `MHProofCache.*`, реестр
endpoint'ов и Asset Registry не изменены — за пределами закрытого списка
контракта правок нет.

## 8. Вопросы

Нет открытых вопросов к владельцу или близнецу. Единственный незакрытый
пункт — реимпорт-бенчмарк 100 placements (acceptance п.6, §6 выше):
не зафиксирован, остаётся полевым тестом owner после merge.

## 9. Финальные проверки в этом worktree

`python tools/check_normative_docs.py` и `git diff --check` прогнаны из
корня этого worktree после написания квитанции и правки tracker-строки;
результат — см. вывод команд, приложенный к этой сессии редактирования
документации.
