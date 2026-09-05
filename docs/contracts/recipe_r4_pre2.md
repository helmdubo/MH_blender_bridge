> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза R4-pre-2 для внешнего исполнителя (близнец, 2026-09-04)

# Контракт R4-pre-2 — Build/Break сохраняют результат: appearance при Break, предупреждения Build

Основание: внешний аудит `docs/reference_notes/dagor_composit_ue5_audit_20260904.md`
§7.B (Build не переносит состояние выбранных акторов) и §7.C (Break мешей теряет
appearance-каналы), оба подтверждены близнецом по коду; решение owner
2026-09-04: Build **предупреждает, не отказывает**. Инвариант аудита §5:
структурная операция сохраняет наблюдаемый результат, а непредставимое
состояние либо переносится, либо явно называется до замены объектов.

Не входит: §7.A (вложенный random-композит после Break — контекст вызова и
resolver) — срез R4-pre-3, близнец.

## Что уже есть в ветке (не переписывать)

Ветка `recipe/r4-pre2-build-break-preserve` от `origin/main` (`ad6d206`). Red-коммит близнеца `511d0ba`:

- `Public/Composite/MHCompositeLevelSubsystem.h` — API-контракт: свободная
  функция `UE::MimirComposite::MHPreflightBuildComposite(Actors, Settings,
  OutDocument, OutWarnings, OutError)`; заглушка в `.cpp` собирает документ как
  сегодняшний Build и **не** выдаёт предупреждений.
- тесты в `MimirCompositeTests/Private/MHCompositeBuildBreakPreserveTest.cpp`
  — **это и есть acceptance**:
  - `Mimir.V5.Composite.Break.MeshLeafKeepsAppearance` — после Break у каждого
    `AStaticMeshActor` `GetCustomPrimitiveData().Data[BaseIndex..BaseIndex+3]`
    равны `AppearanceChannels` соответствующего листа плана (сопоставление по
    world-матрице), `BaseIndex = UMHCompositeSettings::AppearanceCustomDataBaseIndex`;
  - `Mimir.V5.Composite.Build.PreflightWarnsAboutLostState` — preflight
    выделения {обычный managed-меш, managed-меш с `SetMaterial(0, …)` и
    `SetCustomPrimitiveDataFloat`, `AMHCompositeActor`} возвращает true, три
    узла, и предупреждения: для композита — путь актора и слово `seed`; для
    меша с override — путь актора и `material override`, и отдельно `custom
    primitive data`; для обычного меша — ни одной строки с его путём; акторы
    не тронуты.

Тесты — норма среза; не редактируются. Блокирует — STOP + OPEN.

## Норма

### 1. Break переносит appearance (§7.C)

`MHSpawnBreakSpec` для `Mesh` после `SetStaticMesh` применяет
`MHApplyLeafAppearanceCustomData(Component, Leaf, Settings.AppearanceCustomDataBaseIndex)`
с тем же листом плана, из которого взята world-матрица. Для этого
`FMHBreakSpawnSpec` несёт четыре канала (или индекс листа). Композитные дети
(`AMHCompositeActor`) каналы не получают: они строят свой preview сами.
`Actor`-листья — без изменений (transport их не трогает по норме заголовка
`MHCompositeAppearanceTransport.h`).

### 2. Build предупреждает о теряемом состоянии (§7.B)

`MHPreflightBuildComposite` — единственная сборка документа; `BuildComposite`
вызывает его, добавляет его предупреждения в `OutWarnings` и продолжает
публикацию (предупреждение ≠ отказ). Отказ — только там, где и сегодня:
непредставимый transform, unmanaged меш, актор вне реестра.

Строки предупреждений (по одной на актор и элемент, английский, с путём
актора):

| Выделен | Предупреждение |
|---|---|
| `AMHCompositeActor` | `<path>: child composite seeds (Seed=<n>, AppearanceSeed=<m>) are not representable in the recipe; its random subtree re-rolls under the new parent` |
| `AStaticMeshActor` с `OverrideMaterials` (любой ненулевой элемент) | `<path>: material override in slot <i> (<material path>) is not representable in the recipe and is dropped` |
| `AStaticMeshActor` с `GetCustomPrimitiveData().Data` не пустым | `<path>: custom primitive data (<n> floats) is not representable in the recipe and is dropped` |
| `Actor`-лист с любым свойством, отличным от CDO класса (сравнение через `UObject::PropertiesAreIdentical`/`Identical` по `EditAnywhere` UPROPERTY корневого компонента и актора — допускается ограничить `ChangedProperties` при наличии архетипа) | `<path>: instance properties differing from class defaults are not representable in the recipe and are dropped` |

Слова `seed`, `material override`, `custom primitive data` в строках обязательны
(тест ищет их). Коды `MH_W_*` не вводятся (реестр диагностик пиннут 54/20):
это строки Message Log операции Build, как у warnings импорта.

### 3. UI

`ExecuteBuildComposite` (`UI/MHSourceToolMenus.cpp`) показывает предупреждения
preflight'а на странице «Build MH Composite» **до** публикации через
существующий `NotifyOperation`. Диалог подтверждения не добавляется: решение
owner — предупредить, не блокировать.

## Закрытый список файлов

- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`
  — `MHCollectBreakSpecs`/`FMHBreakSpawnSpec`/`MHSpawnBreakSpec` (каналы),
  `MHPreflightBuildComposite` (предупреждения), `BuildComposite` (вызов
  preflight); Edit-сессия, `MHRebuildAllLoadedCompositeActors` — не трогать;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeLevelSubsystem.h`
  — только если нужен приватный helper; публичный API — контракт;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHSourceToolMenus.cpp`
  — только `ExecuteBuildComposite`;
- `docs/receipts/recipe_r4_pre2.md` (новая), `docs/RECIPE_EXECUTION_STATUS.md`
  (строка R4-pre-2).

## Запрещено

- менять тесты, resolver, `MHMaterializeLayout`, `MHCompiledRecipe`, актор,
  компилятор размещения, транспорт appearance, proof, реестры;
- отказывать Build по состоянию из таблицы §2 (решение owner: предупреждение);
- новые коды `MH_E_*`/`MH_W_*`; Asset Registry; `FinishCompilation`;
- `git pull`, стоя на `main`; push в `main`.

## Acceptance

1. Non-unity/no-PCH сборка хоста — Succeeded.
2. Зелёные: `Mimir.V5.Composite.Break.*`, `Mimir.V5.Composite.Build.*`,
   `Mimir.V5.Composite.LevelOperations`, `Mimir.V5.Composite.BuildPreflight*`,
   `Mimir.V5.Composite.AppliedAdmission.*`.
3. Полный NullRHI suite на generic-хосте: 0 Fail, число тестов = 197 + 2
   (если R3a уже смержен) или 196 + 2.
4. `RunUAT BuildPlugin -StrictIncludes` — Success; guarded force-unity — Succeeded.
5. `git diff --check` чисто; `python tools/check_normative_docs.py` — OK.
6. Квитанция `docs/receipts/recipe_r4_pre2.md`: RED/GREEN логи, гейты,
   таблица предупреждений как реализовано, список OPEN.
7. Полевое подтверждение — owner на портфолио после merge (Break меша с
   материалом, читающим appearance-каналы, сохраняет цвет/вариацию; Build с
   вложенным композитом показывает предупреждение в Message Log). Исполнитель
   интерактивных шагов не выполняет.

## STOP + OPEN

Остановиться и записать `OPEN-R4P2-N` в этот файл (что, где, первая
падающая строка, два варианта), закоммитить только это и сообщить близнецу,
если: тест из acceptance нельзя сделать зелёным без правки теста или
запрещённого файла; существующий тест ломается; для сопоставления «спавн-спек
→ лист плана» нужен новый публичный API; нужен новый код диагностики.

## Host и правила git

Хост — свежий по `tools/setup_s6_runtime_host.ps1`, Engine `UE_5.7`, тесты
NullRHI с `-NoAssetRegistryCache -MHGoldenRoot=<repo>/golden`. Не собирать,
пока идёт прогон тестов. Только ветка `recipe/r4-pre2-build-break-preserve`
(`git checkout --detach origin/…` или локальная ветка от неё); никогда
`git pull` на `main`, никогда push в `main`. Один PR в `main`; merge — близнец
после независимой проверки. Интерактивных шагов в редакторе нет.
