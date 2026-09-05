> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза R3b для внешнего исполнителя (близнец, 2026-09-05)

# Контракт R3b — reconcile по дельте интерфейса вместо полного rebuild

Основание: KICKOFF §5 R3 (red-asserts: «targeted reimport меша с тем же
интерфейсом: `actor_rebuild_ms_total == 0`, `recipes_recompiled == 0`;
изменение child-рецепта: `parent_recipes_recompiled == 0`»), `docs/16_recipe_model.md`
§4 (протокол обновлений — таблица «событие → действие → не делать»), §2.2
(пять хэшей/ревизий, R3a MERGED #102), аудит 2026-09-04 §8.E (полный
`RebuildComposite` всех зависимых акторов на любой реимпорт меша).

R3a дал данные: после `Invalidate` + `Resolve` реестр отдаёт
`GetLastInterfaceDelta(Key)`. R3b их потребляет: `MHNotifyGeneratedResourceChanged`
перестаёт вызывать `RebuildComposite` для меш-ключей и вызывает действия §4.

**Правило DECIDED/STOP (KICKOFF §7, owner 2026-09-05)** действует: противоречие
или мелкая проблема внутри закрытого списка, без правки тестов/публичного
API/resolver/норматива/кодов диагностик и с опорой на red-тест — решать на
месте и писать `DECIDED-R3B-N` в этот файл; остальное — STOP + `OPEN-R3B-N`.

## Что уже есть в ветке (не переписывать)

Ветка `recipe/r3b-resource-reconcile` от `origin/main` (`a519e6a`). Red-коммит близнеца `e00d80a`:

- новый файл `MimirCompositeTests/Private/MHResourceReconcileTest.cpp` —
  **acceptance**:
  - `Mimir.V5.Composite.Reconcile.SameInterfaceReimportKeepsBuckets` — receipt
    `SourceHash` меша A изменён, интерфейс тот же, `MHNotifyGeneratedResourceChanged(static_mesh:A)`:
    `GetPlacementRebuildCount()` не растёт, стадия `ResolveCompositePlan` не
    вызывается, `GetRecipeRevision(root)` не меняется, множество
    `GetDerivedComponents()` то же самое (те же объекты), бакет A хранит те же
    инстансы, число листьев прежнее, `GetLastInterfaceDelta(A).bPayload == true`;
    уведомления `material:*` и `texture:*` ничего не перестраивают;
  - `Mimir.V5.Composite.Reconcile.DescriptorChangeMigratesOnlyAffectedBucket`
    — у меша A добавлен material slot, уведомление: дельта `bBucketDescriptor`,
    rebuild count не растёт, бакет B — **тот же объект**, у A есть бакет с
    одним инстансом, число листьев прежнее;
  - `Mimir.V5.Composite.Reconcile.ChildRecipeReimportKeepsParentRecipe` —
    уведомление `composite:child`: ревизия child +1, ревизия root **не**
    меняется, бакет A родителя — тот же объект (rematerialize только
    поддерева вложенного композита);
- переписанное ожидание `Mimir.V5.Composite.Recipe.ActorReimportViaRecipeDependents`:
  уведомление по вложенному мешу с неизменённым интерфейсом (пустая дельта) —
  **не** rebuild.

Тесты — норма; не редактируются.

## Норма

### 1. Маршрутизация в `MHNotifyGeneratedResourceChanged` (`MHCompositePlacementEvents.cpp`)

| Ключ | Действие | Не делать |
|---|---|---|
| `static_mesh` | `Registry->Invalidate(Key)`; затем `Registry->Resolve(Key)` и `GetLastInterfaceDelta(Key)`; для каждого зависимого актора — `Actor->ReconcileEndpoint(Key, Delta)` (§2) | `RebuildComposite`, `Proofs->InvalidateAll()` при пустой дельте (при непустой — как сейчас) |
| `composite` | `Recipes->InvalidateComposite` (как сейчас); зависимые акторы — `Actor->ReconcileRecipe(Key)` (§3) | перекомпиляция родителей |
| `placement_profile` | как сейчас (`InvalidateProfile` + rebuild носителей) | — |
| `material`, `texture` | ничего в placement'ах (§4 строки 4–5); реестр endpoint'ов инвалидировать можно | rebuild |

### 2. `AMHCompositeActor::ReconcileEndpoint(Key, Delta)` — новый private/friend метод актора

По `FMHEndpointInterfaceDelta`, только для бакетов, чей `GetStaticMesh()` —
endpoint ключа (`MHCollectRecipeGraphDependencies` уже говорит, зависит ли
актор):

| Дельта | Действие над затронутыми бакетами |
|---|---|
| пустая (`!Any()`) | ничего |
| `bFirstAdmission` (меш появился, был Invalid) | сегодняшний путь: `RebuildComposite` (§4 «меш появился» — перенос с заглушки; допускается полный rebuild в R3b, заглушки — R4) |
| только `bPayload` / `bBounds` | `MarkRenderStateDirty()` + `UpdateBounds()` бакета; при `bBounds` — обновить кэш bounds актора, если он есть |
| `bBucketDescriptor` | миграция: пересоздать **только** этот бакет через существующий путь компилятора (`PlanViewConfigureBucket`/бакет-ключ) с тем же набором инстансов и appearance; остальные бакеты не трогать; учёт (`DerivedComponents`, `LeafMaterializations`) обновить точечно |
| `bCollisionInterface` | `RecreatePhysicsState()` бакета |
| `bMaterialBinding` | переприменить материалы бакета из меша (`PlanViewConfigureBucket` часть материалов), без пересоздания компонента |

Ни одно действие не вызывает `RebuildPlacement`, `MHMaterializeLayout`,
resolver, proof. `PlacementRebuildCount` не растёт.

### 3. `AMHCompositeActor::ReconcileRecipe(Key)` — child-рецепт

Рецепт вложенного композита перекомпилируется реестром сам при следующем
`Compile` (ревизия уже поднята). Актор: layout всего размещения заново
считать **можно** (это дёшево и нужно — поддерево изменилось), но
материализация должна быть diff'ом: бакеты, чьи листья не изменились
(ресурс + world-матрица + appearance), сохраняют объект компонента. Минимально
достаточная реализация — существующий reseed-diff путь (`RebuildPlacement(bSeedOnly=true)`-подобный),
если он сохраняет объекты неизменившихся бакетов; иначе точечный diff.
`GetRecipeRevision(parent)` не должна расти: `Compile(parent)` — кэш-хит.

### 4. Перф-счётчики

В `FMHReimportPerfReport` добавить `uint64 RecipesRecompiled`,
`uint64 ParentRecipesRecompiled`, `uint64 BucketsRefreshed`, `uint64 BucketsMigrated`
и вывести их в строку `MH_PERF_REIMPORT` (M0-совместимо: новые поля в конце).
Пиннутых тестов на формат строки нет; проверить `Mimir.V5.Composite.Perf.*`.

## Закрытый список файлов

- `Private/Composite/MHCompositePlacementEvents.cpp` (маршрутизация §1);
- `Public|Private/Composite/MHCompositeActor.{h,cpp}` (§2, §3 — новые методы;
  никаких новых `UPROPERTY`);
- `Private/Composite/MHCompositePlacementCompiler.cpp` и `Public/Composite/MHCompositePlacementCompiler.h`
  — только если для миграции одного бакета нужен вынести существующий helper
  (без изменения поведения полного compile);
- `Public|Private/Performance/MHPerformanceTrace.{h,cpp}` (§4);
- `docs/receipts/recipe_r3b.md` (новая), `docs/RECIPE_EXECUTION_STATUS.md`
  (строка R3b), `docs/16_recipe_model.md` §4 — только если формулировка
  строки таблицы расходится с реализацией (DECIDED с обоснованием).

## Запрещено

- менять тесты; resolver; `MHCompiledRecipe.*`; `MHMaterializeLayout.*`;
  `MHProofCache.*`; реестр endpoint'ов (R3a API — данные, не менять);
- Asset Registry, `FinishCompilation`, `LoadObject` вне реестра;
- новые коды `MH_E_*`/`MH_W_*`;
- «reconcile = rebuild под другим именем»: если `PlacementRebuildCount` растёт
  или `ResolveCompositePlan` вызывается на payload-реимпорте — это не решение;
- `git pull`, стоя на `main`; push в `main`.

## Acceptance

1. Non-unity/no-PCH сборка — Succeeded.
2. Зелёные: `Mimir.V5.Composite.Reconcile.*`, `Mimir.V5.Composite.Recipe.*`,
   `Mimir.V5.Composite.DefinitionPool.*`, `Mimir.V5.Composite.Perf.*`,
   `Mimir.V5.Composite.Seed.*`, `Mimir.V5.Composite.Break.*`.
3. Полный NullRHI suite: 0 Fail, число тестов = 199 + 3.
4. `BuildPlugin -StrictIncludes` — Success; force-unity — Succeeded.
5. `git diff --check`, `check_normative_docs.py` — чисто / OK.
6. Квитанция `docs/receipts/recipe_r3b.md`: RED/GREEN, гейты, таблица
   «дельта → действие» как реализовано, замер: реимпорт одного меша при 100
   placements — `MH_PERF_REIMPORT` до/после (`ActorRebuildMsTotal`,
   `BucketsRefreshed`), DECIDED/OPEN.
7. Полевое подтверждение — owner на портфолио после merge (реимпорт меша из
   Blender без смены материалов: композиты не мигают, Outliner не сбрасывает
   выделение).

## STOP + OPEN

`OPEN-R3B-N` — если нужен новый публичный API вне §2–§4, правка теста, новый
код диагностики, или точечная миграция бакета невозможна без изменения
полного compile-пути.

## Host и правила git

Хост — свежий по `tools/setup_s6_runtime_host.ps1`, Engine `UE_5.7`, тесты
NullRHI с `-NoAssetRegistryCache -MHGoldenRoot=<repo>/golden`. Только ветка
`recipe/r3b-resource-reconcile` (или своя `codex/*` от неё — сообщить имя);
никогда `git pull` на `main`, никогда push в `main`. Один PR; merge — близнец.
Интерактивных шагов нет.
