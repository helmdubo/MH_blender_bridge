> Status: HISTORY · Do not use for implementation · Superseded by docs/16_recipe_model.md

# D0b — Поправки к KICKOFF v2 / docs/16_recipe_model.md (v2.1)

> Status: NORMATIVE-PATCH · применяется срезом `recipe/d0b-docs` к текущему
> main (`eba46ec`, R0b смержен). После применения этот файл переносится в
> `docs/archive/` как HISTORY. Источник: второй внешний аудит (2026-09-02),
> сверенный Lead по коду; решения owner 1–4 подтверждены.

Правило применения: каждая поправка — точечная замена в **обоих** документах
(`KICKOFF_PROMPT.md`, `docs/16_recipe_model.md`), не переписывание разделов
целиком. Формулировки ниже — нормативный текст, вставляется как есть.

## П1. OPEN-R-6 закрыт (проверено по коду)

Удалить условную формулировку «проверить, требует ли resolver…» из §3.3 и
§10. Заменить на:

> Текущий `MHResolveCompositePlan` (`MimirCompositeRuntime/Private/Random/
> MHRandomStream.cpp:809`) первым действием вызывает `MHBuildRandomSourceClosure`,
> которое жёстко завершается ошибкой `missing or invalid raw payload hash` для
> любого ресурса полного замыкания, включая невыбранные варианты. Фазовое
> разделение `Layout → Appearance → Proof` **обязательно**; первый шаг R2a —
> его реализация, а не проверка.

OPEN-R-6: статус `закрыт 2026-09-02`.

## П2. OPEN-R-7 решён: ноль tag-запросов в preview

Заменить временное правило OPEN-R-7 на решение:

> Preview-плоскость не делает tag-запросов Asset Registry, в том числе для
> обнаружения duplicate-claim. При двух ассетах на один logical name preview
> резолвит детерминированный путь (§3.2); дубликат обнаруживают
> source-плоскость (`duplicate_claim` в индексе, warning в Message Log) и
> build preflight (`MH_E_AMBIGUOUS_GENERATED_ASSET`, error). Red-assert R0a
> возвращается к `asset_registry_tag_queries == 0`. Тест
> `Mimir.V5.Composite.AppliedAdmission.DuplicateRootClaimBlocksPlanAndBreak`
> мигрирует в preflight-тест в R2c; до R2c остаётся и помечен
> `@migrate:R2c`.

OPEN-R-7: статус `закрыт 2026-09-02`. В R0b/R0c убрать tag-запрос из admission,
если он там остался (проверить `UMHEndpointPrototypeRegistry`).

## П3. Смена `Seed`: `None` = no-op

Заменить строку протокола §4 «Смена Seed» на таблицу:

| `SeedAffectsResult` | Действие |
|---|---|
| `None` | сохранить значение; layout, appearance и хэндлы не трогать |
| `ChildSeedsOnly` | обновить только endpoint'ы, реально потребляющие layout-сид (вложенные рецепты с `bGenerated`) |
| `Transform` | пересчитать трансформы, `Update` хэндлов |
| `Topology` | Layout + diff add/update/remove |

Смена `AppearanceSeed` обновляет только appearance-каналы. `bAutoAppearanceSeed`
означает «сгенерировать один раз при создании/дублировании», не «вычислять из
позиции».

## П4. `PlacementInterfaceHash` разделяется

В §3.2 и §3.8 заменить одно поле на пять:

```
PayloadRevision        геометрия / render resource            → render refresh
BoundsRevision         пространственные bounds                → bounds cache, streaming bounds
BucketDescriptorHash   поля совместимости FISMComponentDescriptor → миграция бакета
CollisionInterfaceHash BodySetup / collision policy / trace companion → recreate physics state
MaterialBindingHash    slots / default materials / override compatibility → reconcile дескриптора материалов
```

Строка протокола §4 «реимпорт меша, интерфейс тот же» → «изменился только
`PayloadRevision` и/или `BoundsRevision`: render/bounds refresh, без
миграции и без rebuild». Термин `PlacementInterfaceHash` удаляется.

## П5. `PreSaveWorld` не строит proof синхронно

Заменить п.1 §3.6 на:

> `PreSaveWorld` **читает** background proof cache (ключ: `RecipeRevision`
> root'а, generation индекса, `ImporterVersion`, `Registry.Revision`) и
> выводит warning по состоянию `Fresh | Stale | Missing | ProofPending |
> Unknown`. Сам proof в `PreSaveWorld` не строится. Синхронно дождаться
> полного proof имеют право только build preflight и runtime snapshot
> admission (явные действия пользователя).

Добавить в §3 компонент `FMHProofCache` (editor subsystem, background task
по `RecipeRevision`), реализуется в R2c.

## П6. Acceptance R1

Заменить red-assert R1 на:

```
waited_mesh_set == selected_compiling_mesh_set
waited_mesh_set ∩ unselected_mesh_set == ∅
```

`all_option_unique_meshes / selected_unique_meshes` остаётся метрикой в
квитанции, не условием.

## П7. Ключ compiled recipe

§3.1: ключ реестра — `UMHCompositeAsset* + RecipeRevision` (`uint32`,
инкремент при реимпорте/`PostEditChange` ассета). `AppliedHash` ассета
хранится в записи как debug/диагностический атрибут и **никогда** не
сравнивается с Source Root в preview.

## П8. Fingerprint узла (уточнение решения owner 3)

§3.7 заменить состав fingerprint на:

```
NodeFingerprint = Hash(
    semantic kind,
    resource key,
    own authored local transform (canonical),
    relevant semantic node data (options set, profile name),
    ParentSemanticFingerprint )
ParentSemanticFingerprint = Hash(kind, resource key, structural role, его ParentSemanticFingerprint)
                             — без authored transform родителя
```

- `DisplayName` **не** входит (presentation-only); рядом хранятся
  `ExpectedLabel/CurrentLabel` для диагностики.
- Canonical-представление трансформа: нормализованный знак кватерниона,
  `-0.0 → 0.0`, фиксированная сериализация float, порядок компонентов, запрет
  NaN/Inf. Raw-байты `FTransform` не хэшируются.
- Правило: изменился authored transform **самого** узла → override не
  применяется, `MH_W_ORPHAN_OVERRIDE_IDENTITY_CHANGED`; изменился transform
  **предка** → override валиден и движется с предком; сменился семантический
  родитель → override не применяется.

## П9. Порядок: R6 после R5

§5: NodeOverrides остаются в R6 **после** R5 (стабильные хэндлы), не «после
R3». Физика и автосборка — производители `FMHNodeOverrideSet`-транзакций после
R6; ядро не различает источник override.

## П10. Execution tracker

Создать `docs/RECIPE_EXECUTION_STATUS.md` (NORMATIVE, в `NORMATIVE_INDEX.md`):

```
D0a   MERGED   #62
M0    MERGED   #63
R0a   MERGED   #64
R0b   MERGED   #65
D0b   NEXT     (этот патч)
R1    BLOCKED BY D0b
S0    READY    (параллельно)
R2a   PLANNED
...
```

Добавить в §0 KICKOFF и §0 docs/16:

> Архитектурный порядок задаёт KICKOFF. Фактическую точку продолжения задаёт
> `docs/RECIPE_EXECUTION_STATUS.md`; перед началом любого среза исполнитель
> читает его и начинает **только** срез со статусом NEXT/READY.

Обновление тракера — обязательная строка acceptance каждого среза.

## П11. Статус docs/16

Заголовок docs/16 заменить на:

```
Document status: NORMATIVE
Target architecture rollout: TRANSITIONAL
Production cutover milestone: R2b
```

## П12. Удалённые термины (дополнение списка §7.4)

Добавить в лексический ноль (удалённые сущности кода): `PlacementInterfaceHash`
(заменён П4). Проверить, что `MHValidateAppliedCompositeRoot` и
`MHLoadAppliedResource` после R0b действительно удалены из кода, иначе — в R0c.

## Acceptance D0b

1. П1–П12 применены к `KICKOFF_PROMPT.md` и `docs/16_recipe_model.md`
   точечными заменами; diff квитанции показывает только затронутые строки.
2. `docs/RECIPE_EXECUTION_STATUS.md` создан и внесён в `NORMATIVE_INDEX.md`.
3. Второй аудит положен в
   `docs/reference_notes/external_audit_recipe_model_v2_20260902.md`.
4. OPEN-R-6, OPEN-R-7 закрыты в docs/16 с датой и ссылкой на этот патч.
5. CI-проверки §7.2 зелёные; грепы §7.4 (с П12) = 0 вне archive/receipts.
6. Этот файл перенесён в `docs/archive/` с HISTORY-заголовком.
7. Квитанция `docs/receipts/recipe_d0b.md`.

Кода нет. Ветка `recipe/d0b-docs`. Исполнитель: Sonnet 5 достаточно —
все решения приняты, замены точечные.
