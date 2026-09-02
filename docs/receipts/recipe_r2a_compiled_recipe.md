# R2a-2 (Recipe Model v2.1) — `FMHCompiledRecipe` и shadow parity как CI-гейт

Статус: **READY FOR REVIEW**. Вторая половина R2a (KICKOFF §5, docs/16 §2.1,
§2.3): плоская программа рецепта, реестр `UMHCompiledRecipeRegistry`,
preview-путь `Layout + Appearance` на графе из скомпилированных рецептов и
постоянный гейт `RecipeShadowParity`. Preview-путь **ещё не production**
(переключение — R2b); production-код по-прежнему идёт через applied graph.

## 1. База и границы

- ветка: `recipe/r2a-compiled-recipe`; база `origin/main` `c0a145e`
  (после merge R2a-1 #79 и тракера #80);
- host близнеца `E:\MimirComposite_R_M0_20260902`;
- новые файлы: `MimirCompositeEditor/Public|Private/Composite/MHCompiledRecipe.{h,cpp}`,
  тест `MimirCompositeTests/Private/MHCompiledRecipeTest.cpp`; ни один
  существующий файл кода не изменён; resolver, `golden/`, wire-формат не тронуты;
  тесты не удалялись.

## 2. Acceptance

| # | Критерий (docs/16 §2.1, §2.3) | Результат |
|---|---|---|
| 1 | `FMHCompiledRecipe`: `TArray<FComponent>` в DFS-порядке, иерархия интервалами `BeginInd/EndInd`, **сырые веса**, **canonical TRS как в источнике**, `TransformKind (Matrix \| Ranges)`, NodePath в грамматике resolver'а | GREEN: `CompiledRecipeStructure` — 4 компонента golden-фикстуры, интервалы `[0,2) [1,2) [2,4) [3,4)`, веса `0/1/3`, TRS `(100,0,0)`, NodePath совпадает с `Plan.Nodes[i].NodePath` |
| 2 | Вложенный композит — **ссылка** на рецепт дочернего ассета по хэндлу; профили inline; компиляция без загрузки мешей | GREEN: `References` = хэндлы `variant_a/variant_b` (asset + их `RecipeRevision`), дочерние рецепты компилируются транзитивно; компилятор читает только `MHExtractCompositeV5` + ключи ресурсов и резолвит **только композиты** через `UMHEndpointPrototypeRegistry` |
| 3 | Ключ реестра `asset + RecipeRevision`; инкремент при `PostEditChange`/реимпорте; `AppliedHash` — debug-атрибут, не сравнивается с Source Root | GREEN: `PostEditChange` → revision+1, `Find` не отдаёт устаревший рецепт, `Compile` пересобирает; дочерние рецепты переживают перекомпиляцию родителя |
| 4 | Цикл — по стеку компиляции, `MH_E_COMPOSITE_CYCLE` | GREEN: A→B→A не компилируется, ошибка содержит код |
| 5 | Обратный индекс `Dependents: ResourceKey → {recipes}` только для локализации rematerialize, не для перекомпиляции родителей | GREEN: `static_mesh:variant_a_mesh` → `{variant_a}`, не root; `composite:variant_a` → `{root}` |
| 6 | **Shadow parity — CI-гейт**: на всех golden-векторах сравниваются decisions, draws, nodes, leaves, матрицы, appearance-каналы, selected dependencies между reference-обёрткой и preview-путём | GREEN: `RecipeShadowParity` — shared fixture (7×7 сидов) + appearance synthetic + 4 сценария (7×7 каждый) = 294 пары, 0 расхождений; preview без closure |
| 7 | Applied-graph parity на реальных generated-ассетах с mesh-receipt (путь diagnostic command `mh.RecipeShadowParity`) | GREEN: `RecipeShadowParityApplied` — 5 сидов × 3 appearance, 0 расхождений |

## 3. Red-first

Тесты `Mimir.V5.Composite.Recipe.{CompiledRecipeStructure, RecipeShadowParity,
RecipeShadowParityApplied}` (`MHCompiledRecipeTest.cpp`). Фикстура теста
превращает golden-граф в generated composite-ассеты по каноническим путям
(имена композитов с per-run суффиксом на обеих сторонах — в ассетах и в
reference-графе, включая ключи `RawHashes`).

RED — коммит `61c8553`: API объявлен, все точки входа fail-closed
(`MH_E_NOT_IMPLEMENTED`). Лог `R2A2_RED_TEST.log`, строки 1079/1086/1098:
`Result={Fail}` для трёх тестов, единственная причина — заглушка; все фикстуры
(shared + 5 appearance) применились без ошибок; `ResolverPhasesParity` (R2a-1)
`Success`.

GREEN — коммит `6c10260` (реализация; первая GREEN-итерация падала только на ридере
тестовой фикстуры без `gameobj`, исправлено в тесте). Лог `R2A2_GREEN2_TEST.log`,
строки 1079–1097: четыре `Result={Success}`.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | RED `Succeeded` (`R2A2_RED_BUILD.log`); GREEN `Succeeded` (`R2A2_GREEN2_BUILD.log`) |
| `Mimir.V5.Composite.Recipe.*` (4 теста) | 4× `Success` (`R2A2_GREEN2_TEST.log:1079-1097`) |
| полный NullRHI suite, generic host | `Success=183`, 0 Fail (`R2A2_FULL.log`) |
| то же под `MimirCompositeV5S6.uproject` (isolated) | `Success=181 Fail=2` (`R2A2_FULL_ISOLATED.log`): только известные pre-existing `Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration` (baseline-доказательство в receipt R0a) |
| `BuildPlugin -StrictIncludes` | `ExitCode=0 (Success)` (`R2A2_STRICT_UAT.log`, пакет `E:\MimirComposite_R_R2A2_Strict_20260902`) |
| guarded force-unity | `Succeeded` (`R2A2_FORCE_UNITY_UBT.log`) |
| `git diff --check` / `check_normative_docs.py` | чисто / OK |

## 5. Реализация

- `FMHCompiledRecipeComponent { NodePath; NodeFingerprint (=0 до R6); Kind;
  Resource; ResourceKey "kind:name"; DisplayName; AuthoredTrs; TransformKind;
  ProfileName; InlinePlacement; bAppearanceSeedBoundary; Options[{Kind,
  Resource, ResourceKey, WeightRaw, NestedRecipe}]; ParentIndex; BeginInd;
  EndInd; NestedRecipe }`; `FMHCompiledRecipe { Asset; LogicalName;
  RecipeRevision; AppliedHashDebug; Components; Profiles; References;
  bGenerated }`.
- `UMHCompiledRecipeRegistry` (editor subsystem): `Compile` (кэш по
  `asset + RecipeRevision`, стек циклов), `Find`, `Invalidate`,
  `GetRecipeRevision`, `GetGeneration` (глобальный счётчик перекомпиляций),
  `GetSeedAffectsResult` (кэш со стампом generation, классификация через
  `MHClassifyCompositeGraph` на recipe-графе), `GetDependents`. Инвалидация:
  `FCoreUObjectDelegates::OnObjectPropertyChanged` и
  `UImportSubsystem::OnAssetReimport`; страховка — `Find` считает рецепт
  устаревшим, если `AppliedHash` самого ассета изменился (сравнение с полем
  ассета, не с Source Root).
- `MHBuildRecipeGraph` — граф из рецептов (узлы восстанавливаются из интервалов,
  профили — с проверкой на расходящиеся carriers, вложенные — через хэндл и
  **текущий** рецепт ребёнка); `MHResolveRecipePreview` = граф → Layout →
  Appearance; `MHCompareRecipeShadowParity` — пофайловое сравнение всех массивов
  плана (double/float — точное равенство, матрицы `==`); `MHRunRecipeShadowParity`
  — applied graph + reference wrapper vs preview.
- Diagnostic command: `mh.RecipeShadowParity <ObjectPath> [Seed] [AppearanceSeed]`
  (лог `LogMHCompiledRecipe`).
- Нормализация весов и схлопывание узлов в матрицу **не делаются** (R8, после
  exhaustive parity), как требует §2.1.

## 6. Изменённые файлы

- `Source/MimirCompositeEditor/Public/Composite/MHCompiledRecipe.h` (new)
- `Source/MimirCompositeEditor/Private/Composite/MHCompiledRecipe.cpp` (new)
- `Source/MimirCompositeTests/Private/MHCompiledRecipeTest.cpp` (new)
- `docs/receipts/recipe_r2a_compiled_recipe.md`, `docs/RECIPE_EXECUTION_STATUS.md`

## 7. Вопросы Lead

- **OPEN-R2A-1 (не блокирует).** `SelectedDependencies` preview-плана строится
  из recipe-графа без `ResourceDependencies` (mesh → material → texture), которые
  applied graph добавляет из живых мешей. На фикстурах и тестовых мешах без
  слотов это совпадает; на реальном ассете с материалами
  `mh.RecipeShadowParity` покажет расхождение **только** в
  `selected dependencies`. Предложение: в R2b preview не использует
  `SelectedDependencies` (это proof-артефакт), и сравнение parity по этому полю
  ограничить ресурсами графа (composite/mesh/profile). Решение за Lead.
