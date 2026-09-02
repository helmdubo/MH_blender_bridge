> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза R0c для внешнего исполнителя (пишет и принимает близнец)

# Контракт R0c — preview без tag-запросов; duplicate claim в proof-плоскости; удаление `MHValidateAppliedCompositeRoot`

Основание: D0b П2 (OPEN-R-7 закрыт), П12; `docs/16_recipe_model.md` §2.2, §2.4, §2.6, §7.2, §9;
`docs/RECIPE_EXECUTION_STATUS.md` (R0c READY).

## Что уже есть в ветке (не переписывать)

Ветка `recipe/r0c-proof-duplicate-claim` от `origin/main`. Red-коммит содержит:

1. `Mimir.V5.Composite.Registry.IdentityAdmission` часть B: assert
   `asset_registry_tag_queries == 0` (было `== uniqueKeys` по временному правилу
   OPEN-R-7). Сегодня red: реестр делает одну tag-пробу на admission.
2. Новый тест `Mimir.V5.Composite.Registry.DuplicateClaimIsProofPlane`
   (`MHEndpointPrototypeRegistryTest.cpp`): два ассета с одним logical name
   (канонический путь + alias-путь), оба зарегистрированы в Asset Registry.
   Ожидания: `RebuildComposite` даёт план (preview резолвит канонический путь);
   `BreakComposites` отказывает с `MH_E_AMBIGUOUS_GENERATED_ASSET`;
   `MHBuildRuntimeCompositeInput` отказывает с тем же кодом; после удаления
   дубликата Break проходит. Сегодня red: план `nullptr`.

RED-лог: `E:\MimirComposite_R_M0_20260902\Saved\Logs\R0C_RED_TEST.log` (строки в квитанции).

## Задача

1. **Реестр** (`MHEndpointPrototypeRegistry.cpp`, `Admit`): удалить блок
   «OPEN-R-7 fail-closed rule» — `FARFilter`/`GetAssets`/`Claims`. Резолв: только
   `FindObject` → `LoadObject` по каноническому пути; `MHRecordEndpointAssetRegistryTagQuery`
   в preview больше не вызывается. Подписка на `OnAssetsAdded/Removed` остаётся
   (инвалидация по тегам события — не запрос).
2. **Proof-плоскость**: новая функция в `MHCompositeResolvedPlan.{h,cpp}`
   `bool MHCheckGeneratedAssetClaims(const FMHResourceKey& Key, FString& OutError)`
   — tag-запрос Asset Registry (разрешён в proof), `>1` заявка на ключ →
   `MH_E_AMBIGUOUS_GENERATED_ASSET: <key>`, одна заявка не по каноническому пути →
   `MH_E_SOURCE_INDEX_INVALID: invalid generated path for <key>`. Вызывать для
   root-композита и каждого ресурса плана в точках выхода: `UMHCompositeLevelSubsystem::BreakComposites`
   (до спавна) и `BuildComposite`, `MHBuildRuntimeCompositeInput` (до сборки bindings).
   `PreSaveWorld` и preflight в этом срезе не трогать (R2c).
3. **`MHValidateAppliedCompositeRoot`** удалить из `MHCompositeResolvedPlan.{h,cpp}`;
   в `UMHCompositeDefinitionSubsystem::GetOrBuildDefinition` заменить вызов на
   inline: `MHAdmitEndpointIdentity(RootKey, Root, OutError)` и
   `UMHEndpointPrototypeRegistry::ResolveEndpoint(RootKey, OutError) == &Root`.
   Символ должен исчезнуть из `ue/` полностью (grep = 0).
4. **Миграция теста** `Mimir.V5.Composite.AppliedAdmission.DuplicateRootClaimBlocksPlanAndBreak`
   (`MHCompositeAppliedPlanAdmissionTest.cpp`): по §7.5 удаляется в этом срезе
   **только** потому, что его замена (`Registry.DuplicateClaimIsProofPlane`, уже в
   ветке) зелёная в том же PR. Удаление — отдельной строкой квитанции с именем
   замены. Остальные три `AppliedAdmission.*` не трогать; `InvalidRootReceiptBlocksPlanAndBreak`
   обязан остаться зелёным (structural admission живого объекта остаётся в
   `MHAdmitEndpointIdentity`).
5. Тракер: строка R0c → `IN REVIEW`, после merge → `MERGED #<PR>`.

## Закрытый список файлов

- `Private/Composite/MHEndpointPrototypeRegistry.cpp`
- `Public/Composite/MHCompositeResolvedPlan.h`, `Private/Composite/MHCompositeResolvedPlan.cpp`
- `Private/Composite/MHCompositeDefinitionSubsystem.cpp`
- `Private/Composite/MHCompositeLevelSubsystem.cpp`
- `Private/Composite/MHCompositeRuntimeBridge.cpp`
- тесты (удаление): `MimirCompositeTests/Private/MHCompositeAppliedPlanAdmissionTest.cpp` (только один тест)
- документы: `docs/receipts/recipe_r0c.md`, `docs/RECIPE_EXECUTION_STATUS.md`,
  `docs/16_recipe_model.md` — только строка таблицы §7.2 для шестой сущности
  (`R2c` → `R0c`).

Red-тесты в `MHEndpointPrototypeRegistryTest.cpp` не менять. Мешает другой
тест — STOP + OPEN в квитанции.

## Запрещено

- любой `GetAssets`/tag-фильтр/`FAssetData(&Object)` в preview-плоскости
  (реестр, closure builder, placement compiler, `RebuildComposite`);
- чтение `ProjectIndex`/Source Root в preview;
- изменение resolver'а (`MimirCompositeRuntime`), `golden/`, Engine;
- параллельный старый путь под флагом.

## Acceptance

1. Оба red-теста зелёные; лексический ноль: `grep -rn "MHValidateAppliedCompositeRoot" ue/` = 0.
2. В `MH_PERF_MAPLOAD` двух акторов одного ассета: `asset_registry_tag_queries=0`,
   `live_receipt_tag_reads=0`, `registry_lookups == uniqueKeys` (строка из лога в квитанции).
3. Полный NullRHI `Automation RunTests Mimir` на generic host — 0 failed; число
   тестов: −1 (`DuplicateRootClaimBlocksPlanAndBreak`) +1 (`DuplicateClaimIsProofPlane`).
4. Гейты KICKOFF §9 (non-unity/no-PCH, force-unity, StrictIncludes, `git diff --check`,
   `check_normative_docs.py`).
5. Квитанция `docs/receipts/recipe_r0c.md` с таблицей удалений тестов и файлов.
6. PR в `main`; merge после проверки близнеца.

## Host исполнителя

См. `docs/contracts/recipe_r1_1.md` §«Host исполнителя».
