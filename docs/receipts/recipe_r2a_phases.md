# R2a-1 (Recipe Model v2.1) — фазовое разделение resolver'а

Статус: **READY FOR REVIEW**. Первая половина R2a (KICKOFF §5): единственная
правка замороженного resolver'а — фазовое разделение
`Layout → Appearance → Proof` без изменения выборок, математики и подписей.
Вторая половина (R2a-2: `FMHCompiledRecipe`, shadow parity как CI-гейт) —
отдельный срез и PR.

Разбиение R2a на два PR — правило «≤6 файлов / ≤7 acceptance»: разрез
resolver'а и компилированный рецепт — независимые red-тесты и разные модули
(Runtime vs Editor).

## 1. База и границы

- ветка: `recipe/r2a-resolver-phases`; база `origin/main` `db43f21`
  (после merge S2 #77 и тракера #78);
- host близнеца `E:\MimirComposite_R_M0_20260902`;
- изменён только `MimirCompositeRuntime/Random/MHRandomStream.{h,cpp}` и один
  тестовый файл; `golden/`, wire-формат, editor-модуль не тронуты; тесты не
  удалялись.

## 2. Acceptance

| # | Критерий | Результат |
|---|---|---|
| 1 | `MHResolveCompositePlan` = `Layout → Appearance → Proof` с прежним публичным контрактом | выполнено: обёртка — композиция трёх фаз |
| 2 | Layout не требует хэшей замыкания (OPEN-R-6 закрыт D0b П1: разделение обязательно) | GREEN: `MHResolveCompositeLayout` на графе без `RawHashes` даёт те же decisions/leaves/матрицы/appearance-каналы, что reference |
| 3 | Паритет фаз с reference на golden-фикстуре, все 7 сидов | GREEN: преимиджи подписей (layout, appearance), `ClosureHash`, три подписи, counts Nodes/Draws/Leaves, per-leaf `WorldMatrix`, `SelectedDependencies` побайтно равны |
| 4 | Proof отказывает без хэшей | GREEN: `MHBuildCompositeProof` → `missing or invalid raw payload hash` |
| 5 | Golden-тесты resolver'а/appearance/gaz53/runtime parity без изменений и зелёные | см. §4 (полный suite) |
| 6 | Гейты KICKOFF §9 | §4 |

## 3. Red-first

Тест `Mimir.V5.Composite.Recipe.ResolverPhasesParity`
(`MimirCompositeTests/Private/MHRandomStreamV5Test.cpp`): для каждого сида
`plan_vectors` golden-фикстуры сравнивает reference и фазовый путь по
преимиджам подписей, `ClosureHash`, трём подписям, счётчикам и per-leaf
матрицам/зависимостям; затем Layout+Appearance на копии графа с пустыми
`RawHashes` против reference и отказ Proof на ней.

Первая редакция теста (`e98ad05`) сравнивала JSON `MHBuildCompositePlanReport`;
этот отчёт требует материализованные компоненты (ошибка «parity report
requires exactly one materialized component per resolved leaf») и в чистом
resolver-тесте неприменим. Коммит `77ffbbe` заменил его сравнением преимиджей
подписей — более строгим (преимидж покрывает все decisions/draws/leaves
байт в байт). RED-утверждение (Layout без хэшей) от этой замены не зависит.

RED — коммит `e98ad05` (API фаз объявлен, Layout — ещё весь reference resolver
с closure). Лог `E:\MimirComposite_R_M0_20260902\Saved\Logs\R2A1_RED_TEST.log`,
строки 1077–1092: `Result={Fail} Name={ResolverPhasesParity}`; паритет фаз
проходит, падают ровно семь `'seed N: layout needs no raw payload hashes'`.

GREEN — коммит `2473545` (разрез) + `77ffbbe` (тест): §4, лог
`R2A1_GREEN2_TEST.log`, строка 1078: `Result={Success} Name={ResolverPhasesParity}`.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | RED `Succeeded` (`R2A1_RED_BUILD.log`); GREEN `Succeeded` (`R2A1_GREEN2_BUILD.log`) |
| `Mimir.V5.Composite.Recipe.ResolverPhasesParity` | `Success` (`R2A1_GREEN2_TEST.log:1078`) |
| полный NullRHI suite, generic host | `Success=180`, 0 Fail (`R2A1_FULL2.log`) |
| то же под `MimirCompositeV5S6.uproject` (isolated) | `Success=178 Fail=2` (`R2A1_FULL2_ISOLATED.log`): только известные pre-existing `Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration` (доказаны на baseline-хосте в receipt R0a) |
| `BuildPlugin -StrictIncludes` | `ExitCode=0 (Success)` (`R2A1_STRICT2_UAT.log`, пакет `E:\MimirComposite_R_R2A1_Strict2_20260902`) |
| guarded force-unity | `Succeeded` (`R2A1_FORCE_UNITY2_UBT.log`) |
| `git diff --check` / `check_normative_docs.py` | чисто / OK |

## 5. Реализация

- `MHResolveCompositeLayout` — прежнее тело `MHResolveCompositePlan` без
  `MHBuildRandomSourceClosure` в начале и без вызова appearance в конце; обход
  дерева, `MHSelectWeightedOption`, `CanonicalTrs`/`ComposeTrs`/`ApplyProfile`
  и порядок записей плана — без изменений.
- `MHBuildCompositeProof` — `MHBuildRandomSourceClosure` +
  `MHRefreshResolvedCompositeSignature` (ResolvedSignature, AppearanceSignature,
  PlacementSignature).
- `MHResolveCompositeAppearance` — без изменений (draws + refresh подписей);
  в обёртке подписи пересчитываются Proof'ом после closure, поэтому итог
  побайтно прежний. Разница только в порядке: closure строится после обхода,
  который его никогда не читал (проверено грепом и компилятором: у Layout нет
  доступа к closure).
- Preview-путь на фазы **не переключён** (это R2b); R2a-2 добавит
  `FMHCompiledRecipe` и shadow parity `RecipeShadowParityTest`.

## 6. Изменённые файлы

- `Source/MimirCompositeRuntime/Public/Random/MHRandomStream.h`
- `Source/MimirCompositeRuntime/Private/Random/MHRandomStream.cpp`
- тест: `Source/MimirCompositeTests/Private/MHRandomStreamV5Test.cpp`
- `docs/receipts/recipe_r2a_phases.md`, `docs/RECIPE_EXECUTION_STATUS.md`

## 7. Вопросы Lead

Нет. OPEN-R-6 закрыт D0b; реализация подтверждает: Layout никогда не читал
closure.
