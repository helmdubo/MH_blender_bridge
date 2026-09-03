> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза R4-pre для внешнего исполнителя (близнец, 2026-09-03)

# Контракт R4-pre — Break композита = один слой рецепта, без proof, Undo из записи

Основание: полевой тест owner `docs/receipts/field_r2b3_break_20260903.md`
(Break медленный, разбирает композит в плоский набор мешей, после Undo — дубли
мешей и утроенные бакеты); ресёрч RS-1
`docs/reference_notes/dagor_composite_build_break_20260903.md` (§2 «Split
composites» снимает ровно один слой, §2.5 undo = записи, §3 почему быстро, §6
сводка «что повторить у нас»); ADR `docs/16_recipe_model.md` §2.10
(транзакционен только актор), §9 OPEN-R-1.

Решения близнеца, принятые для этого среза (делегировано owner 2026-09-03):

1. **Break — операция preview-плоскости**, как «Split composites» в daEditor.
   Он читает резидентный план актора и ничего не доказывает: ни applied graph,
   ни closure, ни claims, ни свежесть источника. Proof остаётся у сохранения
   карты (audit), snapshot и cook (docs/16 §2.6 п.2, п.3 без Break).
2. **Один слой.** Верхний слой рецепта → по одной сущности уровня на компонент;
   вложенный композит остаётся `AMHCompositeActor` со своим ассетом; random →
   только выбранный вариант; группа — структура, не сущность: её дети
   поднимаются на слой; пустой узел/вариант — ничего.
3. **Undo как в Dagor**: транзакция хранит записи (актор композита с `asset,
   seeds, transform` + спавненные акторы); plan-view компоненты **не**
   транзакционны, `PostEditUndo` материализует заново из записи.
4. `BuildComposite`, имена акторов, placement-профили, рекурсивный режим Break,
   пулы (R4) — **не** в этом срезе.

## Что уже есть в ветке (не переписывать)

Ветка `recipe/r4-pre-break-top-layer` от `origin/main` (`28e898d`, после #92 и
RS-1). Red-коммит близнеца `8e6ab13` содержит:

- новый файл `MimirCompositeTests/Private/MHCompositeBreakTest.cpp` — три
  теста, **это и есть acceptance**:
  - `Mimir.V5.Composite.Break.TopLayerOnly` — фикстура root =
    `[mesh A] [random{composite child(mesh C) w1, empty w0}] [composite nested(mesh B)] [group{mesh D}]`,
    актор с `Seed=21, AppearanceSeed=34`, повёрнут и сдвинут. Ожидание: ровно
    4 актора — `AStaticMeshActor(A)`, `AMHCompositeActor(child)`,
    `AMHCompositeActor(nested)`, `AStaticMeshActor(D)`; мешей B и C среди
    результата нет; world-трансформ каждого = `WorldMatrix` резолвленного узла
    × трансформ актора (`MHMatrixElementsWithinTrsTolerance`); у композитных
    детей `Seed=21`, `AppearanceSeed=34`, `bAutoSeed=false`, есть резидентный
    план; исходный актор ретирован.
  - `Mimir.V5.Composite.Break.NoProofNoTagQueries` — после Break
    `MHGetPlacementStageMetrics().Get(BuildAppliedGraph).Calls == 0`,
    `MHGetEndpointResolveMetrics().AssetRegistryTagQueries == 0`,
    `LiveReceiptTagReads == 0`.
  - `Mimir.V5.Composite.Break.UndoRestoresPlacement` — Break → `UndoTransaction`:
    исходный актор жив, число `GetDerivedComponents()`, `GetLeafMaterializations()`
    и ISM-бакетов равно значениям до Break, `PreviewRevision` вырос, у актора
    нет компонентов-сирот (все компоненты кроме root учтены в derived), спавненные
    акторы мертвы; `RedoTransaction` снова ретирует композит. **Внимание:** на red-коммите этот
    тест уже зелёный (дефект №3 полевого теста в автоматическом мире с NullRHI не
    воспроизводится — проверены `OwnedComponents`, все компоненты с outer =
    актор, все зарегистрированные ISM мира). Он — regression-guard для снятия
    `RF_Transactional`. Воспроизведение и закрытие дефекта №3 доказывается
    **в редакторе на карте хоста** (см. Acceptance п.8).
- переписанные под норму одного слоя утверждения существующих тестов:
  `Mimir.V5.Composite.LevelOperations` (random → композитный актор на 150,
  верхний слой = 4 сущности в порядке узлов, вложенный **пустой** композит —
  тоже сущность), `AppliedAdmission.*` (Break отказывает только когда у preview
  нет плана; дефект closure его не блокирует), `Registry.DuplicateClaimIsProofPlane`
  (Break проходит по каноническому endpoint'у без tag-запросов; snapshot
  по-прежнему отказывает).

Тесты — норма среза. Их не редактируют. Если тест невозможно сделать зелёным
без изменения его текста или запрещённого файла — STOP + OPEN (см. ниже).

## Норма

### 1. Preflight Break (до любой мутации)

Для каждого выбранного `AMHCompositeActor`: живой, не template, не в edit-сессии,
не дубликат в выделении (как сейчас); `GetResolvedPlan() != nullptr` и
`GetLastPlacementError().IsEmpty()`, иначе отказ с диагностикой preview
(`MH_E_INVALID_RESOURCE_SOURCE: Break requires a current resolved plan for …` /
текст `LastPlacementError`). Отказ одного актора = отказ всего выделения без
спавна (как сейчас). **Убрать**: `UMHProofCacheSubsystem::BuildProofNow`,
`MHBuildAppliedCompositeGraph`, `MHCheckGeneratedAssetClaims`, любые чтения
Asset Registry tags и живых receipt-тегов. Endpoint'ы берутся так же, как их
берёт preview: `UMHEndpointPrototypeRegistry::ResolveEndpoint` (composite и
static_mesh ключи) — ноль tag-запросов по норме docs/16 §2.2.

### 2. Верхний слой = по грамматике `NodePath` резидентного плана

Обходятся `Plan.Nodes` в порядке плана. Путь узла (`docs/10` §6, R2a):
`<root>:nodes[i]` — компонент верхнего слоя; `…/options[j]` — выбранный
вариант random-узла (тот же слой); `…/children[k]` — ребёнок группы; любой
путь, содержащий `>`, лежит **внутри вложенного композита** и никогда не
порождает сущность. Правила по `SemanticKind` компонента (или его выбранного
варианта):

| Узел | Сущность уровня | Поля |
|---|---|---|
| `Mesh` | `AStaticMeshActor` (существующий `MHSpawnBreakSpec`) | меш — endpoint узла из плана; tm = `WorldMatrix × ActorTransform`; label как сейчас (`DisplayName`, иначе ресурс) |
| `Actor` | актор зарегистрированного класса (как сейчас) | tm, label как сейчас |
| `Composite` | `AMHCompositeActor` | `CompositeAsset` = endpoint composite-ключа ресурса; `bAutoSeed=false`, `Seed` = `Seed` родителя; `bAutoAppearanceSeed=false`, `AppearanceSeed` = `AppearanceSeed` родителя; tm = `WorldMatrix` узла composite × `ActorTransform`; label = `DisplayName` узла, иначе logical name ассета; тот же `ULevel`/folder |
| `Random` | сущность его выбранного варианта (`SelectedOptionIndex`) по строкам выше | tm — узел варианта (у опции нет своего tm: это `WorldMatrix` random-узла) |
| `Group` | ничего; дети группы обрабатываются как компоненты слоя (рекурсивно через вложенные группы) | tm детей уже мировой в плане |
| `Empty` / невыбранный вариант | ничего | — |
| gameobj / placement-профили | как сейчас для листьев верхнего слоя (запечённый tm); не расширять | — |

Никакого обхода `Plan.Leaves` для верхнего слоя. Если для composite-узла
endpoint не резолвится — отказ всего Break до мутации (диагностика endpoint'а,
код существующий). Спавненный `AMHCompositeActor` должен построить preview
штатным путём (`SetCompositeAsset` после сидов); проверять его план в Break не
нужно — это делает тест.

### 3. Транзакция и Undo

Один `FScopedTransaction("Break MH Composite")`; спавн через `GEditor->AddActor`
(транзакционные акторы), затем `Modify` + `EditorDestroyActor` исходных (как
сейчас). Изменить учёт plan-view компонентов:

- `MHCompositePlacementCompiler.cpp`: `PlanViewFlags` **без `RF_Transactional`**
  (компоненты — транзиентные производные записи, docs/16 §2.10);
- `AMHCompositeActor::PostEditUndo` (и, если нужно, `PostEditImport`) —
  восстановление материализации **из записи**: после Undo актор ретирует все
  производные компоненты, которые он не учитывает (в том числе восстановленные
  транзакцией, если такие остались), и строит preview заново штатным путём
  (реестр рецептов + `MHMaterializeLayout`), без proof. Результат: число
  derived-компонентов, листьев и бакетов равно исходному, компонентов-сирот нет.

Если снятие `RF_Transactional` ломает существующий тест (edit-сессия, Undo
перемещений, `Ctrl+Z cannot resurrect a pre-Commit placement edit`) — не
чинить тест: STOP + OPEN с именем теста и первой падающей строкой.

### 4. Стоимость

Break — O(число компонентов верхнего слоя) спавнов + O(1) чтений плана на
компонент. Никаких построений графов, closure, `FinishCompilation`, загрузок
ресурсов сверх того, что уже резидентно в preview. В квитанции — время Break
одного композита из полевой сцены owner, если доступна, иначе синтетика
(100 листьев в 3 слоях: до/после).

### 5. Документы

- `docs/16_recipe_model.md`: §2.6 п.3 — убрать `BreakComposites` из
  синхронных `BuildProofNow`; §2.10 — абзац «Break: preview-плоскость, один
  слой, дети-композиты с сидами родителя, plan-view компоненты не
  транзакционны, `PostEditUndo` восстанавливает из записи»; в «OPEN-R-7 —
  контекст» убрать «и Break» из перечня отказывающих точек; §9 — строка
  `OPEN-R4P-1` (ниже) со статусом «открыт, fail-closed: сиды родителя».
- `docs/receipts/recipe_r4_pre.md` по образцу `docs/receipts/recipe_r2c.md`:
  база, acceptance-таблица, RED/GREEN логи, гейты, изменённые файлы, вопросы.
- `docs/RECIPE_EXECUTION_STATUS.md`: строка R4-pre → IN REVIEW с номером PR.

## Закрытый список файлов

- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`
  — только Break-путь (`MHCollectBreakSpecs`, `MHSpawnBreakSpec`,
  `MHDestroySpawnedActors`, `BreakComposites`, их helpers); `BuildComposite`,
  Edit-сессия, `MHRebuildAllLoadedCompositeActors` — не трогать;
- `…/Public/Composite/MHCompositeLevelSubsystem.h` — только если нужен новый
  private helper;
- `…/Private/Composite/MHCompositePlacementCompiler.cpp` — только `PlanViewFlags`
  и то, что напрямую следует из снятия `RF_Transactional`;
- `…/Public|Private/Composite/MHCompositeActor.{h,cpp}` — только
  `PostEditUndo`/`PostEditImport`/учёт производных компонентов после Undo;
  новых `UPROPERTY` нет (см. OPEN-R4P-1);
- `docs/16_recipe_model.md` (только §2.6 п.3, §2.10, контекст OPEN-R-7, §9),
  `docs/receipts/recipe_r4_pre.md` (новый), `docs/RECIPE_EXECUTION_STATUS.md`
  (строка R4-pre).

## Запрещено

- менять тесты (`MimirCompositeTests/**`), включая «поправить ожидание»;
- resolver (`MHRandomStream.*`), `MHCompiledRecipe.*`, `MHMaterializeLayout.*`,
  `MHProofCache.*`, `MHEndpointPrototypeRegistry.*`, `MHCompositePlacementEvents.cpp`,
  runtime-мост, UI (`MHSourceToolMenus.cpp` вызывает Break как сейчас);
- `BuildComposite`, placement-профили, имена акторов, рекурсивный Break
  (чекбокс), любые новые пункты меню;
- новые коды `MH_E_*`/`MH_W_*` (реестр диагностик пиннут тестом 54/20 — новый
  код только через STOP + OPEN);
- Asset Registry tags, `FAssetData(&Object)`, `FinishCompilation`, `LoadObject`
  ресурсов внутри Break сверх endpoint-реестра;
- параллельный «старый» путь Break (флаг/cvar «как раньше»);
- сохранение на акторе-ребёнке чего-либо кроме `CompositeAsset`, сидов, tm
  (никаких «родительских» ссылок, префиксов путей, маркеров «из композита» —
  docs/16 §7.1, ресёрч §4);
- `git pull`, стоя на `main`; push в `main`.

## Acceptance

1. Non-unity/no-PCH сборка хоста
   (`Build.bat UnrealEditor Win64 Development -Project=<host>\HostProject.uproject -Plugin=… -NoHotReload -NoEngineChanges -DisableUnity -NoPCH -NoSharedPCH -WarningsAsErrors`)
   — Succeeded.
2. Зелёные: `Mimir.V5.Composite.Break.TopLayerOnly`,
   `Mimir.V5.Composite.Break.NoProofNoTagQueries`,
   `Mimir.V5.Composite.Break.UndoRestoresPlacement`,
   `Mimir.V5.Composite.LevelOperations`, `Mimir.V5.Composite.AppliedAdmission.*`,
   `Mimir.V5.Composite.Registry.*`, `Mimir.V5.Composite.Recipe.*`,
   `Mimir.V5.Composite.Proof.*`.
3. Полный NullRHI suite на generic-хосте: 0 Fail, число тестов = 191 + 3;
   на isolated `MimirCompositeV5S6.uproject` — только известные pre-existing
   `Lifecycle.NoBuildBeforeRegistration`, `Seed.AppearanceMigration`.
4. `RunUAT BuildPlugin -StrictIncludes` — Success; guarded force-unity — Succeeded.
5. `git diff --check` чисто; `python tools/check_normative_docs.py` — OK
   (включая лексический код-гейт docs/16 §7.2).
6. Grep-гейт среза (в квитанции):
   `grep -n "BuildProofNow\|MHBuildAppliedCompositeGraph\|MHCheckGeneratedAssetClaims" MHCompositeLevelSubsystem.cpp`
   не находит ни одного вхождения в Break-пути;
   `grep -n "Plan.Leaves\|->Leaves" MHCompositeLevelSubsystem.cpp` — ноль в
   `MHCollectBreakSpecs`/`BreakComposites`;
   `grep -n "RF_Transactional" MHCompositePlacementCompiler.cpp` — ноль.
7. Квитанция `docs/receipts/recipe_r4_pre.md` с логами RED/GREEN, гейтами,
   таблицей изменённых файлов, замером Break (§4) и списком OPEN.
8. Полевое воспроизведение дефекта №3 на хосте (интерактивный редактор, карта с
   сохранённым `AMHCompositeActor` ≥ 2 слоёв, Break из меню «Break MH Composite»,
   Ctrl+Z): **до** правок — зафиксировать число компонентов/бакетов актора
   (`Inspect resolved plan` / Details) и наблюдение «два меша, клик выделяет
   оба»; **после** — те же числа равны исходным, дублей нет. Если до правок
   дефект не воспроизводится или его причина отличается от диагноза
   `field_r2b3_break_20260903.md` §2 (восстановленные транзакцией plan-view
   компоненты + rebuild в `PostEditUndo`) — STOP + OPEN с наблюдениями, не
   «чинить наугад».

## OPEN-R4P-1 — точное воспроизведение вложенного композита после Break

В Dagor ребёнок-композит после split воспроизводит себя точно: он получает
`rndSeed` = бегущий сид родителя на момент `selectEnt` (ресёрч §2.3). У нас
потоки резолвера ключуются `NodePath` от корня (`<root>:nodes[k]>child:nodes[i]`);
самостоятельный актор ребёнка резолвит `child:nodes[i]` — другие потоки при
том же `Seed`. **Fail-closed правило среза**: ребёнок получает `Seed` и
`AppearanceSeed` родителя как есть (тесты проверяют именно это); его внутренний
random может выбрать иные варианты, чем были видны в родителе. Варианты для
решения Lead (не реализовывать в R4-pre): (а) сохранить на акторе-ребёнке
префикс пути потоков и передавать его в layout как необязательный параметр
resolver'а (правка resolver — только близнец, R4); (б) принять
ре-рандомизацию как норму. До ответа — правило выше.

## STOP + OPEN

Любая из ситуаций ниже — остановиться, ничего не «обходить», написать в
`docs/contracts/recipe_r4_pre.md` раздел «OPEN-R4P-N» (что, где, первая
падающая строка, два варианта), закоммитить только это и сообщить близнецу:

- красный тест из acceptance нельзя сделать зелёным без правки теста или
  запрещённого файла;
- существующий тест (любой) ломается снятием `RF_Transactional` или изменением
  семантики Break;
- нужен новый код диагностики, новое `UPROPERTY` на акторе, новый параметр
  resolver'а/materialize;
- `GEditor->AddActor` спавн `AMHCompositeActor` требует чего-то, чего нет в
  закрытом списке.

## Host и правила git

Хост — свежий по `tools/setup_s6_runtime_host.ps1` (никогда не портфолио
owner), Engine `UE_5.7`, тесты NullRHI с `-NoAssetRegistryCache
-MHGoldenRoot=<repo>/golden`. Не собирать, пока идёт прогон тестов (общий UBT
mutex и залоченные DLL). Работать только в ветке
`recipe/r4-pre-break-top-layer` (`git checkout --detach origin/…` или своя
локальная ветка от неё); коммиты обычные (без force-push, без squash чужих);
никогда не делать `git pull` на `main`, никогда не пушить `main`. PR в `main`
один; merge — близнец после независимой проверки на своём хосте.

## OPEN-R4P-2 — интерактивный Undo-gate недоступен исполнителю

**Контекст.** На нетронутом HEAD `39fd4f8` (red-коммит `8e6ab13`) отдельный
module-free host `E:\MimirComposite_R4Pre_External_20260903\HostProject.uproject`
собран non-unity/no-PCH (`R4P_RED_BUILD_NONUNITY_02.log`, `Result: Succeeded`).
Red-фильтр `Mimir.V5.Composite.Break.*` воспроизвёл контрактные падения:
`NoProofNoTagQueries` — `R4P_RED_BREAK.log:1090`, один вызов
`BuildAppliedGraph`; `TopLayerOnly` — строки 1102–1107, flatten в четыре меша
при нуле дочерних composite-акторов; `UndoRestoresPlacement` — Success.

До первой правки запущен обычный интерактивный UE 5.7 editor: процесс отвечает,
имеет окно `HostProject - Unreal Editor`, а лог подтверждает
`Transaction tracking system initialized` и `Engine is initialized`. Но
доступный исполнителю UI-провайдер возвращает `apps: []`; первая попытка
получить native-app поверхность завершается `cua.listApps is not a function`,
точечная привязка к
`D:\PersonalProjects\UE5\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe` —
`cua.getApp is not a function`. Доступны только browser surfaces. Поэтому
исполнитель не может выполнить обязательные действия Acceptance 8 через
интерактивное меню `Break MH Composite` и `Ctrl+Z`, проверить клик/двойное
выделение или снять сопоставимые до/после наблюдения. Это ограничение среды,
а не свидетельство того, что дефект не воспроизводится.

**Вопрос.** Каким способом закрыть обязательный интерактивный Acceptance 8 до
начала реализации?

**Варианты.**

1. Близнец выполняет и публикует полевое воспроизведение до правок на своей
   сохранённой карте, затем после implementation-коммита — повторный прогон;
   исполнитель продолжает только после получения исходного evidence.
2. Исполнителю предоставляется native UI-control для Unreal Editor и готовая
   сохранённая карта ≥2 слоёв. Если вместо UI допускается editor-only C++
   field harness, это должно быть отдельным нормативным изменением Acceptance
   8 близнецом: текущий текст явно требует меню и `Ctrl+Z`.

**Временное fail-closed правило.** Production-файлы, тесты, docs/16, tracker и
квитанция не меняются; green-реализация и PR не создаются до ответа.

**Статус:** OPEN, STOP.
