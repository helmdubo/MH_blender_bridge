# Composite drag + viewport selection — execution receipt, 2026-09-07

История исполнения, не норматив и не замена полевого acceptance owner.
Контракт: `docs/contracts/composite_drag_selection.md`; база `b99ff394`, ветка
`codex/composite-drag-selection`. Пользователь сообщил улучшение loading/Save
после предыдущего среза, затем запросил исправление драга и выбор вложений.

## Воспроизведение

Собственные host: `E:/temp/MH_CEI1_host_20260907` и strict host
`E:/temp/MH_CEI1_package_20260907/HostProject`, UE 5.7.4 CL51494982.
Логи ниже находятся в первом каталоге, если не указано иначе.

`DRAG_RED_BUILD.log`: guarded non-unity/no-PCH SUCCESS. `DRAG_RED.log`:
прежний baseline прошёл; `GestureNotificationsAreBatched` RED — 100 событий
OnChanged вместо 0 внутри 100 mouse samples, более одного события после End.
Baseline: 3497.73 ms / 100 updates, **34.977 ms/update**, 6 компонентов.
Время синтетической сцены, не измерение пользовательского cottage.

`SELECTION_RED_BUILD.log`: SUCCESS. `SELECTION_RED.log`: прежний owner-route
прошёл, `PoolLeafSelectsNearestCompositeOccurrence` RED — выделялся весь owner
вместо одного sibling occurrence, неверны повторный клик, right-click и
random composite option, логический leaf не очищался при deselect.

`DRAG_PERF_BEFORE.log`: оба trace-теста прошли. MAPLOAD: 0 sync package loads,
0 tag queries, 0 waits, total 0.132 ms; targeted reimport: 0 full scans,
0 parent recompiles, 1 notified actor. Один startup FileManager warning/error
о переносе старого CachedAssetRegistryDiscovery.bin вне тестов; test report 0 failed.

## Изменения

Interactive transforms обновляют draft и проекцию без per-delta OnChanged.
EndTracking публикует одно уведомление для изменившегося жеста. Кэш графа
проекции получает authored TRS без canonical serialization, recipe compilation,
endpoint lookup и appearance writes. Существующий resolver и admission проверяют
результат; topology mismatch возвращает полное обновление. Неизменные матрицы
компонентов не записываются. Resolver пока проходит весь граф placement:
этот срез не вводит отдельную математику разрешения поддерева.

Viewport reverse lookup сохраняет точный leaf и ближайшее composite occurrence.
Native selection остаётся owner actor, pool outline ограничен его occurrence.
Edit Contents использует снимок leaf из контекстного меню, открывает найденное
определение и выбирает authored node через уже существующие projection bindings.
Composite Outliner показывает то же вхождение. Камера не перехватывается.

## Проверки

`DRAG_GREEN_BUILD.log` SUCCESS; `DRAG_GREEN.log` / `DragGreenReport` —
**52/52** EditMode. 100 updates 20.91 ms, **0.209 ms/update** на тех же
6 компонентах, против 34.977 ms/update baseline. Native/session observers
молчат во время жеста; graph build counter не растёт; forced full refresh
совпадает с fast path, включая процедурного потомка. Это около 167× на
синтетическом CPU hot path, не обещание такого FPS в пользовательской сцене.

`DRAG_SELECTION_GUARDED.log` SUCCESS, `DRAG_SELECTION_STRICT.log` SUCCESS.
Первый полный `DRAG_SELECTION_FULL.log` / `DragSelectionFullReport` —
290/291. Новый helper-тест обнаружил существующий дефект projection binding:
`AuthoringPathForVisual` обрезал `/options[k]` в адресе родительского вхождения.
Теперь option stripping ограничен суффиксом текущего определения; отдельный
обходной выбор GUID в helper не используется. Финальные результаты ниже.

- `DRAG_SELECTION_UNITY.log`: force-unity/adaptive-off SUCCESS.
- `DRAG_SELECTION_STRICT_FINAL.log`: final non-unity/no-PCH SUCCESS.
- `DRAG_SELECTION_FINAL.log` / `DragSelectionFinalReport`: **291/291**,
  0 failed, оба RecipeShadowParity прошли. Итоговый baseline 0.126 ms/update
  при 5 компонентах (выбранный вариант fixture зависит от GUID имени).
- `DRAG_SELECTION_RHI.log` / `DragSelectionRHIReport`: **63/63**, 0 failed,
  D3D12 / RTX 3070, strict DLLs; EditMode, Selection, cold loading, dependencies,
  Save proof policy и оба RecipeShadowParity. Game View видимость и outline
  сохранились. Проверены native selection seam и projection binding; физический
  ввод мышью на пользовательской карте остаётся полевой проверкой.
- `DRAG_PERF_AFTER.log` / `DragPerfAfterReport`: **2/2**. MAPLOAD 0.097 ms,
  0 sync loads/tag queries/waits; reimport 0 full scans/parent recompiles,
  1 notified actor. Выбор вариантов отличается от before, поэтому это проверка
  пути и счётчиков, не сравнительный процент ускорения загрузки.
- `git diff --check`, `python tools/check_normative_docs.py`: OK.

Новых зависимостей и удалённых тестов нет. Пакет и installation receipt:
`E:/temp/MH_drag_selection_20260907/` (manifest, SHA256, FIELD_CHECK и
installation.json). Старая версия сохраняется в PluginBackups вне Plugins.

## Полевая проверка

1. Кликнуть меш гаражного подкомпозита: подсвечивается его содержимое целиком,
   соседние вхождения остаются без подсветки.
2. Правый клик → Composite Options → Edit Contents: открывается этот
   подкомпозит, узел задетого меша уже выбран, камера сохраняет положение.
3. Перемещение, вращение и масштабирование; Undo/Redo; Cancel или один Esc.
4. Повторить с Save и с другим вхождением того же определения; проверить
   выбор самого root actor через World Outliner и открытие корневого Edit.
