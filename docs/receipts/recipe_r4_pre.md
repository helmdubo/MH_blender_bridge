# R4-pre — Break верхнего слоя рецепта без proof

Статус: **READY FOR REVIEW**. Break переведён в preview-плоскость и снимает
ровно один слой резидентного плана; вложенные композиты остаются
`AMHCompositeActor`, plan-view компоненты не транзакционны, Undo заново
материализует preview из записи актора. Evidence «после» на портфолио снимает
owner до merge по Acceptance 8 контракта.

## 1. База, ветка и host

- ветка: `recipe/r4-pre-break-top-layer`; merge-base с `origin/main`:
  `28e898d`;
- red-коммит близнеца: `8e6ab13`; контрактный HEAD после закрытия
  OPEN-R4P-2: `ad839f6`;
- green implementation: `41b52e6`;
- отдельный worktree:
  `E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge_r4pre_executor`;
- собственный module-free host:
  `E:\MimirComposite_R4Pre_External_20260903\HostProject.uproject`;
  stock UE 5.7.4: `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён
  junction'ом к R4-pre worktree;
- дополнительный isolated host:
  `E:\MimirComposite_R4Pre_Isolated_20260903\MimirCompositeV5S6.uproject`;
- основной checkout `main`, Engine, Blender addon, `golden/` и `reference/`
  не изменялись; `main` не пушился.

## 2. Red-first

Нетронутый контрактный HEAD сначала собран обязательной командой
non-unity/no-PCH:

```text
E:\MimirComposite_R4Pre_External_20260903\Saved\Logs\R4P_RED_BUILD_NONUNITY_02.log
2096: Result: Succeeded
```

Первым тестовым действием после сборки был
`Automation RunTests Mimir.V5.Composite.Break`:

```text
E:\MimirComposite_R4Pre_External_20260903\Saved\Logs\R4P_RED_BREAK.log
1087: Test Completed. Result={Fail} Name={NoProofNoTagQueries}
1090: Expected 'Break builds no applied graph' to be 0, but it was 1.
1099: Test Completed. Result={Fail} Name={TopLayerOnly}
1102: Expected two top-level meshes to be 2, but it was 4.
1103: Expected two nested composites to be 2, but it was 0.
1104–1107: selected/nested composite missing; nested meshes C/B flattened.
1118: Test Completed. Result={Success} Name={UndoRestoresPlacement}
```

RED совпал с контрактом: старый Break строил proof и обходил все
`Plan.Leaves`; regression-guard Undo уже был зелёным в NullRHI, как явно
зафиксировано контрактом.

## 3. Реализация

- `BreakComposites` требует только живой current resident plan и current
  cached compiled recipe; `BuildProofNow` и proof-объекты из Break удалены.
- `MHCollectBreakSpecs` обходит `Plan.Nodes` в порядке плана. Пути с `>`
  пропускаются; группы не создают сущность и поднимают дочерние узлы;
  random читает `SelectedOptionIndex` и соответствующую опцию cached recipe.
- Mesh и composite endpoint'ы разрешаются через
  `UMHEndpointPrototypeRegistry::ResolveEndpoint`; composite spawn получает
  asset, transform, folder и оба сида родителя при отключённых auto-seed,
  причём `SetCompositeAsset` вызывается после сидов.
- `PlanViewFlags` больше не содержит `RF_Transactional`.
- `PostEditUndo` сначала очищает производный view. Очистка включает не только
  transient tracking array, но и все MH-tagged instance components, затем
  штатный `RebuildComposite` заново строит preview из записи актора.

## 4. Green и acceptance

Focused Break:

```text
E:\MimirComposite_R4Pre_External_20260903\Saved\Logs\R4P_GREEN_BREAK_01.log
1087: Test Completed. Result={Success} Name={NoProofNoTagQueries}
1098: Test Completed. Result={Success} Name={TopLayerOnly}
1111: Test Completed. Result={Success} Name={UndoRestoresPlacement}
1118: TEST COMPLETE. EXIT CODE: 0
```

Расширенный composite-прогон:

```text
E:\MimirComposite_R4Pre_External_20260903\Saved\Logs\R4P_GREEN_COMPOSITE_01.log
1170/1190/1210: AppliedAdmission.* — Success
1708/1717/1726: Proof.* — Success
1734–1794: Recipe.* — Success
1803/1815: Registry.* — Success
2047: Test Completed. Result={Success} Name={LevelOperations}
Итого: 89/89 Success, 0 Fail; 2173: TEST COMPLETE. EXIT CODE: 0
```

| # | Критерий | Результат |
|---|---|---|
| 1 | non-unity/no-PCH, warnings as errors | PASS — `R4P_GREEN_BUILD_NONUNITY_01.log:153760`, `Result: Succeeded` |
| 2 | Break.*, LevelOperations, AppliedAdmission.*, Registry.*, Recipe.*, Proof.* | PASS — focused 3/3 и composite 89/89 |
| 3 | полный generic и isolated NullRHI suite | PASS — generic **194/194 Success, 0 Fail**, `R4P_GREEN_FULL_01.log`, последний completed `:4865`, `TEST COMPLETE` `:4877`; isolated также **194/194 Success, 0 Fail**, `R4P_ISOLATED_FULL_01.log`, последний completed `:4638`, `TEST COMPLETE` `:4650` |
| 4 | force-unity | PASS — `R4P_GREEN_BUILD_FORCEUNITY_FINAL_01.log:6`, `Result: Succeeded` |
| 4a | StrictIncludes | PASS — `R4P_GREEN_STRICTINCLUDES_FINAL_01.log:221`, `BUILD SUCCESSFUL`; `:223`, ExitCode 0 |
| 5 | `git diff --check` / normative docs | PASS / PASS — `normative docs: OK` |
| 6 | grep-гейты | PASS — Break range 613–727: proof-вызовы 0; collect/break `Plan.Leaves` 0; placement compiler `RF_Transactional` 0 |
| 7 | квитанция, файлы и OPEN | PASS — этот документ; OPEN-R4P-1 сохранён |
| 8 | Undo: кодовый разбор + автоматический guard | PASS для executor — разбор §6; `UndoRestoresPlacement` Success; field «после» остаётся owner до merge |

## 5. Сборочные и статические гейты

| Gate | Результат |
|---|---|
| final non-unity/no-PCH editor build, `-NoEngineChanges -WarningsAsErrors` | PASS — `R4P_GREEN_BUILD_NONUNITY_01.log:153760` |
| guarded force-unity, точная команда с `-MaxParallelActions=4` | PASS — `R4P_GREEN_BUILD_FORCEUNITY_FINAL_01.log:6`; перед этим UBA достиг memory threshold при открытом owner-editor, два оставшихся unity TU безопасно достроены serial/no-UBA |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | PASS — `R4P_GREEN_STRICTINCLUDES_FINAL_01.log:157/187/216`, все targets `Result: Succeeded`; `:221`, `BUILD SUCCESSFUL`; `:223`, ExitCode 0; package `E:\MimirComposite_R4Pre_Strict_20260903_2152_ConfigSerial` |
| isolated `MimirCompositeV5S6Editor` build | PASS — `R4P_ISOLATED_BUILD_01.log:152`, `Result: Succeeded` |
| `git diff --check` | PASS — `R4P_GREEN_STATIC_GATES_01.log:1` |
| `python tools/check_normative_docs.py` | PASS — `R4P_GREEN_STATIC_GATES_01.log:3–4`, `normative docs: OK` |
| тесты не менялись | PASS — `R4P_GREEN_STATIC_GATES_01.log:8`, diff от `8e6ab13` по `MimirCompositeTests/**` пуст |

Первый StrictIncludes запуск попал в защитный UBA memory-loop при открытом
owner-editor и был прерван без изменения исходников. Для финальной точной UAT
команды пользовательский UBT config временно задавал обычный executor и один
action; после `BUILD SUCCESSFUL` исходный пустой config восстановлен с тем же
SHA-256 `85382F0E3B2029D752E3CC42EF17F2C1727E1700D55D2451F050C474BA3772FB`,
временная backup-копия удалена. Owner-editor и его процесс не закрывались.

Grep-гейт среза выполнен по Break-функции и её collect helper: запрещённых
`BuildProofNow|MHBuildAppliedCompositeGraph|MHCheckGeneratedAssetClaims` — 0;
`Plan.Leaves|->Leaves` — 0. В
`MHCompositePlacementCompiler.cpp` `RF_Transactional` — 0. Одноимённые
proof-вызовы в `BuildComposite` не относятся к Break и контрактом не менялись.
Числа grep-гейта записаны в `R4P_GREEN_STATIC_GATES_01.log:5–8`.

Оба разрешённых isolated pre-existing исключения в этом host не проявились:
`Lifecycle.NoBuildBeforeRegistration` — Success
(`R4P_ISOLATED_FULL_01.log:3607`), `Seed.AppearanceMigration` — Success
(`:3995`). Break-тесты в isolated suite — Success на строках 3238/3249/3262.

## 6. Undo и путь дублей

Evidence «до» принято из owner-квитанции
`docs/receipts/field_r2b3_break_20260903.md` §1–§2. До R4-pre
`PlanViewFlags` включал `RF_Transactional`: транзакция Break сохраняла не
только запись `AMHCompositeActor`, но и его производные plan-view компоненты.
Undo восстанавливал эти компоненты, тогда как transient-массивы
`DerivedComponents`, `TopLevelPlacementComponents` и строки materialization
не являлись надёжной записью восстановления. `PostEditUndo` одновременно
вызывал `RebuildComposite`, а дополнительный editor construction pass мог
собрать view ещё раз. Результат полевого пути: восстановленные транзакцией
компоненты + новый rebuild (+ construction rebuild), неучтённые близнецы и
утроенные ISM-бакеты.

После R4-pre этот путь невозможен для новых view: plan-view компоненты не
участвуют в транзакции. Единственная запись Undo — asset, сиды и transform
актора. Дополнительный защитный рубеж в `PostEditUndo` сначала собирает все
MH-tagged instance components, включая неучтённые компоненты старого
transaction-era view, ретирует их и очищает transient plan state; только затем
штатный recipe/materialization path строит один новый preview. Автоматический
guard `Break.UndoRestoresPlacement` подтверждает исходные counts derived,
leaves и ISM buckets, отсутствие orphan-компонентов и зелёный Redo. Полевое
evidence «после» по договору снимает owner до merge.

## 7. Стоимость Break

Owner-сцена исполнителю не устанавливалась, поэтому использована разрешённая
контрактом синтетика: три уровня `root composite → 10 nested composites → 100
mesh leaves`. Одноразовый внешний host-модуль сравнил replay старого
`BuildProofNow + Plan.Leaves + AStaticMeshActor` пути с production
`BreakComposites`; helper-файлы не входят в репозиторий или PR. Каждый прогон
проверил 100 старых mesh-акторов против 10 дочерних composite-акторов и
завершился `Test Completed. Result={Success}`:

| Прогон | Старый proof + flatten, мс | Новый preview one-layer, мс | Лог (`bench` / `completed`) |
|---|---:|---:|---|
| 1 | 231.7979 | 25.3056 | `R4P_BREAK_SYNTHETIC100_NESTED_1.log:1180/1193` |
| 2 | 260.8283 | 32.6848 | `R4P_BREAK_SYNTHETIC100_NESTED_2.log:1180/1193` |
| 3 | 222.6796 | 24.4947 | `R4P_BREAK_SYNTHETIC100_NESTED_3.log:1180/1193` |
| 4 | 211.1731 | 24.9588 | `R4P_BREAK_SYNTHETIC100_NESTED_4.log:1180/1193` |
| 5 | 222.5073 | 25.2342 | `R4P_BREAK_SYNTHETIC100_NESTED_5.log:1180/1193` |

Медиана: **222.6796 → 25.2342 мс (8.82×)**. Это измерение изолирует именно
R4-pre: старый путь доказывает и спавнит все 100 листьев, новый читает resident
plan и создаёт только 10 сущностей верхнего слоя; preview каждого дочернего
композита строится штатно внутри измерения нового пути.

## 8. OPEN-вопросы

- **OPEN-R4P-1 — открыт по контракту.** Ребёнок получает `Seed` и
  `AppearanceSeed` родителя как есть; из-за смены корневого `NodePath` его
  внутренний random может выбрать иной вариант. В R4-pre resolver и новые
  persisted поля не добавлялись.
- **OPEN-R4P-2 — закрыт близнецом 2026-09-03**, коммит `ad839f6`:
  интерактивный прогон исполнителя снят; before-evidence — owner receipt,
  after-evidence снимает owner на портфолио до merge.

Новых OPEN-вопросов нет.

## 9. Изменённые файлы

Implementation (`41b52e6`):

- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementCompiler.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeActor.cpp`;
- `docs/16_recipe_model.md` — только §2.6, §2.10, контекст OPEN-R-7 и §9.

Квитанция/статус:

- `docs/receipts/recipe_r4_pre.md`;
- `docs/RECIPE_EXECUTION_STATUS.md` — R4-pre → `IN REVIEW`.

Все файлы входят в закрытый список контракта. Тесты не менялись и не
удалялись; resolver, compiled recipe, materialize, proof cache, endpoint
registry, placement events, runtime bridge, UI, BuildComposite, Engine,
Blender addon, `golden/` и `reference/` не тронуты.

## 10. Трекер и PR

Строка трекера: `R4-pre | IN REVIEW (внешний исполнитель) | #93`.
PR #93 открыт из `recipe/r4-pre-break-top-layer` в `main`; merge выполняет
только близнец после обязательной owner-проверки Undo на портфолио.
