# CE-6b2 — удаление legacy Composite Edit

Статус: **MERGED — PR #166** (owner 2026-09-08). Пользователь принял взаимодействие
Composite Edit и исправленный Break, явно запросил удаление старой реализации,
commit/push и merge в `main`. Merge выполнен: `ad69756`, cleanup-коммит `6155b53`.
Проверки завершены: два полных прогона 292/292, D3D12 97/97, guarded/strict
non-unity/no-PCH и force-unity/adaptive-off, PerfTrace до/после. Квитанция:
`docs/receipts/composite_edit_cleanup.md`.

## Единственный путь редактирования

Корневой Edit и Edit вложенного определения используют одну цепочку:
`UMHCompositeLevelSubsystem` → `UMHCompositeEditSession` → транзакционный
`UMHCompositeEditDocument` → временная `UMHCompositeEditProjection` →
`UMHCompositeEditorMode`. Draft сессии — единственный изменяемый документ;
проекция отображает его и связывает визуальные компоненты с NodeId.

Удаляются переключатель `bCompositeEditModeV2`, legacy actor edit handles,
`SetPlacementEditMode` и связанные edit API, копии `EditingDocument` /
`EditingGraph` на размещении, синхронизация draft из actor handles и
редактирование трансформов через `AMHCompositeActor::Tick`. Это не удаление
пуловых instance handles или обновлений действующей проекции режима.

Удаляются глобальный Slate preprocessor старого Enter/Apply пути и
недостижимые ветки самостоятельного Composite Outliner: выбор владельца из
native actor/SMInstance selection, выбор legacy handles и вход в Edit из
отдельной панели. Неиспользуемые resolver/wrapper API убираются вместе с ними.

## Сохраняемое поведение

- Вне Edit остаётся native actor/secondary selection. Secondary selection
  подсвечивает отдельный объект; Edit открывает содержащее его определение и
  может сразу выбрать соответствующий авторский узел. Трансформы узлов
  изменяются только внутри Edit.
- Composite Outliner остаётся внутри левой панели Edit Mode. Его дерево
  закреплено за активной сессией, даже при пустом native selection или выборе
  проекции. Без открытой сессии widget не показывает выбранный placement.
  Команды draft и модельные `FindForComponent` / `FindForInstance` сохраняются.
- Один drag — одна Undo-транзакция. Undo/Redo внутри Edit восстанавливают
  draft и проекцию. Save публикует без второго подтверждения; Cancel и Esc
  немедленно отменяют сессию. Enter не публикует. Режим сохраняет контроль
  пользователя над камерой и Game View.
- Проверки запрета смены seeds опираются на активную сессию subsystem,
  а не на удалённый флаг редактирования актора.
- Break по-прежнему снимает ровно один слой. Его Undo/Redo сохраняют
  геометрию и соседние placements, включая общий ISM-пул.

Wire-формат, RNG/reference resolver, runtime backend и source semantics этим
cleanup не меняются. Старые receipts и внешние аудиты сохраняются как история;
упоминания доступного legacy backend в них не означают действующий fallback.

## Проверка и завершение

Legacy-тесты переводятся на session/draft/projection либо удаляются с явным
указанием действующего покрытия. Например,
`Outliner.InstanceSelectionRetention` заменён проверками session-only widget
в `EditMode.Structure.OutlinerPinsActiveSessionRoot`.

Полный regression suite содержит **292 вместо 297** тестов до cleanup;
миграции и замены перечислены в квитанции. Обе сборки прошли 292/292,
D3D12 — 97/97; golden/parity и сборочные гейты пройдены. Merge выполнен по разрешению owner: PR #166, `ad69756`.
