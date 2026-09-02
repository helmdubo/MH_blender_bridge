# R2b-1 (Recipe Model v2.1) — `MHMaterializeLayout`

Статус: **READY FOR REVIEW**. Первая часть R2b (KICKOFF §5 R2b, docs/16 §2.5):
чистая функция материализации layout'а над скомпилированным рецептом.
Аддитивный срез: preview-путь актора **ещё не переключён** (R2b-2), ничего не
удалено.

Разбиение R2b на три PR:

| Часть | Содержание | Статус |
|---|---|---|
| R2b-1 | `MHMaterializeLayout` + тесты паритета/чистоты/admission | этот PR |
| R2b-2 | `AMHCompositeActor` строит preview через рецепт + `MHMaterializeLayout`; резидентный план вместо повторного resolve; `PostEditMove` без Layout, загрузка карты без Proof (счётчики стадий) | следующий |
| R2b-3 | удаления proof-состояния актора (`ResolvedSignature`, `CompactResolvedState` как гейт, `PlacementDependencies`, `AppliedGraph`, `AppliedDefinition`) и старых тестов подписей — только после ревью Lead и зелёных `Registry.IdentityAdmission` + preflight full-closure теста (KICKOFF §5 R2b; §7.5 «замена до удаления») | после ответа Lead |

## 1. База и границы

- ветка: `recipe/r2b-materialize-layout`; база `origin/main` `0816922`
  (после follow-up R2a #83);
- новые файлы: `MimirCompositeEditor/Public|Private/Composite/MHMaterializeLayout.{h,cpp}`,
  тест `MimirCompositeTests/Private/MHMaterializeLayoutTest.cpp`, общий
  тестовый заголовок `MimirCompositeTests/Private/MHRecipeTestFixture.h`
  (вынесен из `MHCompiledRecipeTest.cpp` без изменений логики);
- `MHCompiledRecipe.{h,cpp}`: публикация `MHRecipeResourceKey` (была
  file-local); resolver, актор, компилятор размещения, `golden/` не тронуты;
  тесты не удалялись.

## 2. Acceptance

| # | Критерий (docs/16 §2.5) | Результат |
|---|---|---|
| 1 | `MHMaterializeLayout(recipe, Seed, AppearanceSeed, ActorTransform) → FMHLeafPlacement{Kind, ResourceKey, NodePath, NodeFingerprint, WorldMatrix, AppearanceChannels, bOverridden}` + Warnings | GREEN: структура по §2.5; `NodeFingerprint = 0` и `bOverridden = false` до R6 |
| 2 | Паритет с reference-обёрткой: листья (kind, resource, origin, матрица × трансформ актора, каналы, индексы) на всех golden-фикстурах × 7 сидов × 3 трансформа актора | GREEN: `MaterializeLayoutParity` — 5 фикстур × 7 сидов × 3 трансформа = 105 материализаций, 0 расхождений |
| 3 | Чистая функция: не грузит, не спавнит, не читает мир, не считает хэши | GREEN: фикстуры не имеют ни одного endpoint-ассета (меши/классы не существуют), материализация успешна; резидентный план без closure и подписей |
| 4 | Transform admission как у компилятора размещения (`MH_E_UNREPRESENTABLE_TRANSFORM`) | GREEN: `MaterializeLayoutAdmission` — сдвиг (поворот актора × неравномерный масштаб листа) отвергается, ничего не материализуется |
| 5 | Гейты KICKOFF §9 | §4 |

## 3. Red-first

Тесты `Mimir.V5.Composite.Recipe.{MaterializeLayoutParity, MaterializeLayoutAdmission}`.

RED — коммит `9fde789`: API объявлен, функция возвращает
`MH_E_NOT_IMPLEMENTED`. Лог `R2B1_RED_TEST.log`, строки 1087/1097:
`Result={Fail}` для обоих тестов, единственная причина — заглушка; четыре
теста R2a после выноса фикстуры — `Success`.

GREEN — коммит `c792c4d`. Первая GREEN-итерация (`R2B1_GREEN_TEST.log`)
падала по двум причинам из §5 (подписи preview-плана; сдвиг в третьем
трансформе / представимый probe) — исправлены контракт preview и тест, не
resolver. Лог `R2B1_GREEN2_TEST.log`, строки 1081–1111: шесть `Result={Success}`.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | RED `Succeeded` (`R2B1_RED_BUILD.log`); GREEN `Succeeded` (`R2B1_GREEN2_BUILD.log`) |
| `Mimir.V5.Composite.Recipe.*` (6 тестов) | 6× `Success` (`R2B1_GREEN2_TEST.log:1081-1111`) |
| полный NullRHI suite, generic host | `Success=185`, 0 Fail (`R2B1_FULL.log`) |
| то же под `MimirCompositeV5S6.uproject` (isolated) | `Success=182 Fail=3` (`R2B1_FULL_ISOLATED.log`): две известные pre-existing (`Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration`) + **flake** `Lifecycle.AppearanceCustomDataTransport` (см. §8); повтор ×3 на isolated: 3/3 `Success` (`R2B1_FLAKE_ISOLATED_{1,2,3}.log`) |
| `BuildPlugin -StrictIncludes` | `ExitCode=0 (Success)` (`R2B1_STRICT_UAT.log`, пакет `E:\MimirComposite_R_R2B1_Strict_20260902`) |
| guarded force-unity | `Succeeded` (`R2B1_FORCE_UNITY_UBT.log`) |
| `git diff --check` / `check_normative_docs.py` | чисто / OK |

## 5. Реализация

`MHMaterializeLayout` = `MHResolveRecipePreview` (recipe-граф → Layout →
Appearance) → `MHValidateResolvedPlacementTransforms(plan, ActorTransform)` →
листья: `WorldMatrix = Leaf.WorldMatrix × ActorTransform`, ключ ресурса
`MHRecipeResourceKey`, каналы appearance копируются как есть. Ошибки грамматики
получают префикс `MH_E_COMPOSITE_GRAMMAR:` как в `RebuildPlacement`.
`FMHMaterializeResult::Plan` — резидентный план (для Outliner/reseed-diff в
R2b-2), без closure и подписей.

Две нормы, уточнённые первой GREEN-итерацией (оба раза правился тест/контракт
preview, не resolver):

- `MHResolveRecipePreview` **обнуляет** строки `ResolvedSignature`,
  `AppearanceSignature`, `PlacementSignature`: appearance-стадия пересчитывает
  их из преимиджей даже без closure, и без closure они не proof-артефакты и не
  должны выглядеть таковыми. Преимиджи остаются (parity, диагностика).
- Неравномерный масштаб актора под повёрнутыми узлами — сдвиг, который
  `MHValidateResolvedPlacementTransforms` отвергает и у reference-пути (UE
  собирает `S·R`, масштабы без поворота между ними коммутируют, с поворотом —
  нет). Это случай admission-теста, а не паритета; паритет гоняется на
  identity, повороте с переносом и равномерном масштабе с поворотом.

## 6. Изменённые файлы

- `Source/MimirCompositeEditor/Public/Composite/MHMaterializeLayout.h` (new)
- `Source/MimirCompositeEditor/Private/Composite/MHMaterializeLayout.cpp` (new)
- `Source/MimirCompositeEditor/Public|Private/Composite/MHCompiledRecipe.{h,cpp}` (публикация ключа)
- `Source/MimirCompositeTests/Private/MHRecipeTestFixture.h` (new, вынос фикстуры)
- `Source/MimirCompositeTests/Private/MHMaterializeLayoutTest.cpp` (new)
- `Source/MimirCompositeTests/Private/MHCompiledRecipeTest.cpp` (использует общий заголовок)
- `docs/receipts/recipe_r2b_materialize_layout.md`, `docs/RECIPE_EXECUTION_STATUS.md`

## 7. Вопросы Lead

- **OPEN-R2B-1 (блокирует только R2b-3).** KICKOFF §5 R2b: старые тесты
  подписей на акторе удаляются после зелёных
  `PrototypeRegistryIdentityAdmissionTest` (есть: `Registry.IdentityAdmission`)
  и `BuildPreflightFullClosureTest`. Такого теста в дереве нет; ближайшие —
  `Mimir.Audit.MainBaseline.BuildPreflightRejectsBeforeMutation` и
  `Mimir.V5.Runtime.Input.FullClosure`. Предложение: считать гейтом preflight
  full-closure тест, который создаёт R2c (точки выхода), а в R2b-2 proof-поля
  актора оставить **диагностикой без гейтов** (не влияют на rebuild), удалить
  их в R2b-3 после R2c. Альтернатива: написать `BuildPreflightFullClosureTest`
  в R2b-3 самому и удалять сразу. Решение за Lead.

## 8. Находка: flaky-тест вне среза

`Mimir.V5.Composite.Lifecycle.AppearanceCustomDataTransport`
(`MHCompositePlacementLifecycleTest.cpp:475`) упал один раз на isolated-хосте:
`Expected 'the tint value actually moved' to be unequal to 0.809339, but it
was 0.809308 and within tolerance 0.000100`. Тест сравнивает канал appearance
до и после `AppearanceSeed + 1` через `TestNotEqual(float)` с допуском 1e-4;
сид и NodePath (GUID-суффикс имени композита) случайны на каждый прогон, так
что два соседних сида изредка дают значения ближе 1e-4. К R2b-1 отношения не
имеет (актор `MHMaterializeLayout` ещё не использует; на generic-хосте и в
R2a-2 isolated тест зелёный). Предлагаемая правка (отдельный PR, тест-only):
сравнивать `RawU32` draw'ов или использовать точное неравенство.
