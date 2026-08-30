# 14 — Программа UE Editor после S6.1: owner-коррекция роли

Статус: **owner-решения 1–3 ратифицированы 2026-08-29 в рабочей сессии**;
нарезка срезов в §3 — предложение Lead по материалам внешнего аудитора,
требует отдельного owner freeze перед исполнением. Документ не отменяет
10/11/12/13; он фиксирует три новых программных решения и их следствия.

## 1. Решения owner (2026-08-29)

1. **UE5 — одновременно Asset Viewer и Editor.** Blender — место создания
   ассетов: перепаковка, иерархия, реэкспорт, смена текстур, назначение
   материалов. UE5-сцена — место просмотра и редактирования размещений
   composite. Следствие: семантический Composite Outliner и выбор узла кликом
   (аналог даговского `dataBlockId`) — **часть роли, а не превью-комфорт**.
   Данные (`Leaf.Origin`/`NodePath`) уже есть в плане; не хватает
   hierarchy-metadata плана и UI.
2. **Shared definition pool — приоритет.** Взаимодействие с композитами в
   UE5 сейчас дороже даговского: per-actor applied graph против одного
   `CompositEntityPool` на ассет у Dagor. Целевая экономика: на N размещений
   одного composite — **одна** сборка/валидация замыкания и N дешёвых
   per-placement resolve.
3. **Растворение рецепта (S7 Cook flattening) отложено к v2.0**, но
   закладывается в фундамент уже сейчас (§2). Цель v1 — рендер портфолио в
   UE5 из dagor-ассетов; v2.0 — своя игра / отдельный FAB-плагин.

## 2. Фундамент-инварианты растворения

Обязательны для всех срезов начиная с текущих, чтобы v2.0 не потребовал
переделки сущности:

1. Всё, что нужно для bake, выводимо из `FMHResolvedCompositePlan`.
   Plan-first сохраняется; **никаких BeginPlay-only входов** в новых фичах.
2. Новые фичи не создают внешних зависимостей на живой wrapper-actor:
   никто не ссылается на `AMHCompositeActor`/`AMHRuntimeCompositeActor`
   как на gameplay-объект.
3. Runtime carrier (`GraphBytes`) не получает новых обязанностей; новые
   метаданные живут в asset и plan, не в runtime input.
4. Пара сидов (Choice/Appearance, начиная с S6.3) полностью разрешима на
   cook-этапе.
5. Идентичность листьев (`NodePath`/`Origin`) сохраняется в любом
   baked/broken выходе как провенанс: Break и будущий Bake обязаны давать
   один и тот же набор конечных объектов из одного и того же плана.

## 3. Предлагаемая нарезка (после S6.3; требует owner freeze)

```text
V5-S6.4  Semantic plan hierarchy + Composite Outliner + selection
V5-S6.5  Shared compiled definition cache
V5-S7    Cook flattening — PARKED → v2.0
```

**S6.4.** Расширить resolved plan производной hierarchy-metadata:
`ParentResolvedNodeIndex`, `SourceNodeIndex`, `SelectedOptionIndex`,
`OwningResolvedNodeIndex` у листа — derived traversal metadata **вне**
frozen signature preimage (RNG parity не трогается). Composite Outliner:
source tree + resolved overlay (выбранный random-вариант, sampled TRS,
missing endpoint, провенанс). Выбор кликом: normal mode → hit по листу
выбирает владеющий actor; edit mode → hit выбирает семантический узел
(hit proxy → NodePath). Эти же поля закрывают фундамент-инвариант §2.5.

**S6.5.** `UMHCompositeDefinitionSubsystem`: immutable compiled closure с
ключом `(root ResourceKey, root AppliedHash, ClosureHash,
ActorClassRegistry revision, importer version)`; actor получает shared
graph и держит только seeds/transform/plan. Основа —
`proposals/shared_composite_preview_cache.md`. Acceptance: 100 размещений
одного composite → 1 сборка замыкания, 100 resolve; hot reload —
одна инвалидация definition, пересбор всех размещений от общего результата.

Порядок S6.4/S6.5 owner может поменять; содержательных зависимостей нет
(S6.4 упрощает верификацию S6.5 наглядным overlay).

**UX-референс S6.4 — `AGroupActor` (2026-08-30, разбор внешнего агента +
чтение исходников UE 5.7).** Заимствуются паттерны, не data model:
locked/edit двухрежимность (`IsSelected()` locked-группы → «hit по листу =
выбран актор целиком»), brackets через PDI/`SDPG_Foreground` с цветом по
состоянию (locked/edit/unresolved), агрегация
`GetActorBounds`/`GetStreamingBounds`, repair-дисциплина
`PostLoad`-fixup / `FixupGroupActor` / самоликвидация в `PostRemove`,
разнесение actor ↔ `UActorGroupingUtils` → наш
`UMHCompositeEditorSubsystem`. Из `ActorGroupingUtils` дополнительно
(2026-08-30): дисциплина «каждая пользовательская операция = гейт
активности → именованная `FScopedTransaction` → делегирование в
механику → Slate-notification при отказе, без модалок»; паттерн
veto-реестра `FCanGroupActors` (именованные фильтры) — в запас для
будущих node-операций, заранее не заводить; семантика
`UngroupActors` (сначала внешняя locked-группа, потом родитель) —
референс для проверки Break на вложенных композитах: ломается внешняя
обёртка, один уровень за вызов. Зафиксированный бесплатный факт:
`AMHCompositeActor` естественно совместим со штатной Ctrl+G-группировкой
(дельта применяется к актору, basis-update двигает листья) — закрепить
тестом, veto не требуется. НЕ переносится: наследование от
`AGroupActor`, membership как hierarchy, `CenterGroupLocation` (двигает
authored pivot; в Utils вызывается на каждом Add/Remove — тем более),
`GroupApplyDelta` (решает мульти-актора; у нас размещение — один актор).
Важная гипотеза для красного теста S6.4: клик-выбор ломает не отсутствие
group-механики (UE и так роутит hit компонента в владеющего актора), а
дефект превью-компилятора (регистрация компонентов до назначения
StaticMesh / пустые bounds).

**UX-референс S6.4b — редактирующий контур даговского Composite
Outliner (скриншоты owner 2026-08-30, zil130).** Секции панели:
Entities (варианты random: имя + Weight + Insert/Remove/+, пустой
вариант отображается как `--` — наш `empty`), Children
(добавить/вставить/убрать узел), Node parameters (чекбокс «Use
transformation matrix» = переключатель фиксированной tm ↔ random
p2-трансформов; Add/Remove/Copy/Paste сырых параметров), Composit
(Save changes / Reset to file), Delete node. Маппинг на нашу
грамматику: options/веса/empty, children, delete, фиксированный TRS,
change asset — 1:1 через канонический писатель; режим random
p2-трансформов = OPEN-V5-15 (заблокирован — переключатель до решения
не реализуем); сырые script-параметры вне грамматики (у нас только
place_type/appearance_seed_boundary). **Ратифицировано owner
2026-08-30: модель редактирования — сессионная, без автосохранения;
запись в источник только явным «Save changes» (существующий
explicit-overwrite publish), «Reset to file» откатывает к файлу.
Это же решает NodePath-идентичность: структурные правки меняют пути
узлов и производные random-потоки, применяются одним коммитом.**

## 4. Референсы

Внешние источники Dagor (исходники, документация, ключевые пути) и
локальные снапшоты/патчи собраны в
`docs/reference_notes/dagor_engine_references.md`.

## 5. Вне программы

- Исполнение прижатия, обратная запись в Dagor — не будет (owner).
- Level Instance (V5-S8) — parked, без изменений.
- Расширение overlay dag4blend — отклонено owner 2026-08-29 (док 13,
  «Поправки» п.3).
