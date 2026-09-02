# R0b (Recipe Model v2) — все call-site'ы на реестр, фасады удалены

Статус: **READY FOR REVIEW**. Вторая половина R0: чистый рефакторинг
call-site'ов без изменения поведения — после R0a реестр уже обслуживал каждый
резолв через фасады.

## 1. База и границы

- ветка: `recipe/r0b-registry-callsites`; база `origin/main` `a6d7059`
  (после merge R0a #64);
- host исполнителя `E:\MimirComposite_R_M0_20260902` (см. `recipe_m0.md` §1);
- Engine, `golden/`, `reference/`, Blender-аддон, resolver, runtime-мост не
  изменялись; тесты не изменялись и не удалялись.

## 2. Acceptance (KICKOFF v2 §5, строка R0; часть R0b)

| # | Критерий | Результат |
|---|---|---|
| 1 | `MHLoadAppliedResource` и `MHResolveCompositeDefinitionEndpoint` удалены из кода | `grep -rn` по `ue/MimirComposite/Source` = 0 (включая тесты); удалена и мёртвая карта `FMHCompositeDefinitionEntry::Endpoints` |
| 2 | Break (`UMHCompositeLevelSubsystem`), runtime snapshot (`MHCompositeRuntimeBridge`), Outliner overlay, placement compiler резолвят через реестр напрямую | `UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, Error)` — единственная точка резолва endpoint'а в editor-модуле |
| 3 | Поведение не меняется | полный suite без изменений в ожиданиях (§3) |
| 4 | Лексический гейт 16 §7.2 для этих символов | активные документы: 0 упоминаний вне таблицы §7.2 (`check_normative_docs.py`) |

## 3. Red-first

Срез — удаление и переименование без наблюдаемого изменения: после R0a фасады
делегировали реестру, поэтому runtime-red невозможен по построению. Red-эквивалент
— проверка отсутствия символов (§2.1) на коммите до патча (`a6d7059`: 22
вхождения в 8 файлах) и после (`f034e03`: 0). Behaviour-гейт — полный suite.

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH editor build (`-NoEngineChanges -WarningsAsErrors`) | `Result: Succeeded` (`R0B_BUILD_NONUNITY.log`) |
| полный NullRHI `Automation RunTests Mimir`, generic host (`HostProject.uproject`) | **175/175 Success**, 0 failed (`R0B_FULL.log`) |
| то же под именем `MimirCompositeV5S6.uproject` | 173 Success, 2 Fail — те же pre-existing `AssetCheck`-провалы, что в `recipe_r0a.md` §4 (воспроизведены на `origin/main`) (`R0B_FULL_ISOLATED.log`) |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | `ExitCode=0 (Success)` (`R0B_STRICT_UAT.log`) |
| guarded force-unity | 12/12 actions, `Result: Succeeded` (`R0B_FORCE_UNITY_UBT.log`) |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | `normative docs: OK` |

## 5. Изменённые файлы

Модификации (4): `MHCompositePlacementCompiler.cpp` (5 сайтов → 1 вызов
каждый; `MHRecordDefinitionEndpointResolve` в fallback-ветках убран — считает
реестр), `MHCompositeLevelSubsystem.cpp`, `MHCompositeRuntimeBridge.cpp`,
`UI/MHCompositeOutlinerModel.cpp`; `MHEndpointPrototypeRegistry.{h,cpp}` —
статический `ResolveEndpoint`.
Удаления: `MHCompositeResolvedPlan.{h,cpp}` (фасад и объявление),
`MHCompositeDefinitionSubsystem.{h,cpp}` (фасад, объявление, `Endpoints`).

## 6. Вопросы Lead

Нет. OPEN-R-7 (R0a) остаётся открытым; R0b его не касается.
