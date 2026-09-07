# Composite native selection + Edit panel — receipt, 2026-09-07

История исполнения, не норматив. Ветка `codex/composite-native-selection`,
база `859dd0a`; контракт `docs/contracts/composite_native_selection.md`.
Owner заменил RMB-углубление на native double-click toggle и затем добавил
перенос Composite Outliner внутрь левой панели Edit Mode.

## Поведение и реализация

Primary (LMB/RMB) сохраняет logical уровень, Secondary переключает root ↔
occurrence при одном выбранном owner. Вычисление идёт до native selection,
применение — после native Clear/Select/Notify. Одноразовый pending result
проверяет set/owner/frame и итоговый exclusive owner. Для RMB на уже выбранном
owner меню завершает тот же результат, если OnChanged не возник. Меню больше
не выбирает вложение независимо от текущего уровня.

Actor customization UE не заменён; правила групп, блокировки и модификаторов
остаются native Actor-командами. Multi-actor selection очищает logical scope.
Служебный pool actor/component/instance не выбирается. Геометрия, drag, source
и Edit session не менялись. Outliner widget встроен в GetInlineContent режима;
Nomad tab spawner и регистрация удалены. Дерево, node commands и Details
переиспользуют существующий widget и привязку к активной сессии.

Проверено по локальным UE5.7.4 C++ исходникам:

- `LevelEditor/Private/Elements/Component/ComponentElementLevelEditorSelectionCustomization.cpp:67`
  — actor/component predicate и FromSecondary.
- `UnrealEd/Private/ViewportSelectionUtilities.cpp:119` — Primary/Secondary,
  Clear/Select/Notify, redundant RMB и вызов context menu.
- `LevelEditor/Private/LevelEditorContextMenu.cpp:719` — меню по selection.
- Native Actor customization private/unexported; customization registry
  не позволяет безопасно забрать его ownership для decorator. Поэтому
  перенос правил Actor в плагин не потребовался.

## Проверки

Собственные host из прошлых срезов; UE5.7.4 CL51494982, BuildId47537391.
Логи: `E:/temp/MH_CEI1_host_20260907/`.

- RED: `NATIVE_RED_BUILD2.log` SUCCESS; `NATIVE_RED.log`,
  NativePrimarySecondaryCycle — FAIL на прежней production selection:
  Secondary не выбирает вложение, Primary не сохраняет/меняет nested scope.
- Первый GREEN: `NATIVE_GREEN_BUILD2.log` SUCCESS, `NATIVE_GREEN.log` — 1/1.
- После этого прежние RMB-token assertions в SelectionAdapterTest заменены
  независимой проверкой occurrence mapping. Имена/число старых тестов сохранены;
  их click-cycle replacement — NativePrimarySecondaryCycle (GREEN выше).
- Новый тест дополнен root RMB/Edit, nested RMB/Edit + authored preselection,
  whole-occurrence highlight/shared pool isolation, direct actor selection,
  multi-owner reset и inline toolkit content при наличии toolkit host.
- В ходе компиляции исправлены явный include Selection.h в новом тесте,
  расположение delegate subscription в constructor адаптера и namespace
  вызова factory в toolkit. До установки исправления пересобраны.

Финальные гейты:

- `NATIVE_STRICT.log`: StrictIncludes/non-unity/no-PCH SUCCESS.
- `NATIVE_GUARDED.log`: guarded non-unity/no-PCH SUCCESS.
- `NATIVE_UNITY.log`: force-unity/adaptive-off SUCCESS.
- `NATIVE_FULL.log`, NativeFullReport: 292/292, 0 failed, оба
  RecipeShadowParity GREEN. Старые тесты не удалены, добавлен один.
- `NATIVE_RHI.log`, NativeRHIReport: D3D12 offscreen smoke 64/64, 0 failed
  на DLL strict host. Это automation, не ручная проверка пользовательского UI.
- `NATIVE_BEFORE.log` / `NATIVE_AFTER.log`: по 2/2; mapload
  0.142 → 0.124 ms, sync loads/registry lookups/proof work/created components
  0 → 0, reused components 2 → 2. Targeted reimport full scans 0 → 0,
  actor rebuild 0 → 0, notified actors 1 → 1; totals
  149.469/119.645 → 132.039/135.514 ms. Одиночные timings — диагностика,
  не сравнительный бенчмарк.
- Edit baseline fixture: cold enter 4.73 ms, warm enter 4.36 ms,
  100 updates 12.51 ms (0.125 ms/update), 50 cycles 236.74 ms,
  projection components 6. Это синтетическая fixture, не cottage.
- `git diff --check`, `tools/check_normative_docs.py`: PASS.

READY FOR FIELD TEST. Manifest пакета фиксирует source/DLL hashes и commit;
installation.json фиксирует фактическую установку, проверку и путь backup.

## Полевой сценарий

1. LMB cottage → RMB по любому его mesh → Edit Contents: редактируется root.
2. После Cancel: LMB cottage → double LMB garage → подсвечен весь garage.
3. Single LMB/RMB по sibling: подсвечивается его occurrence; Edit открывает
   выбранное определение с выбранным authored node кликнутого leaf.
4. Double LMB при выбранном вложении: возврат к root.
5. В Edit слева дерево Composite Outliner, node commands и Details;
   отдельного пункта окна Outliner нет. Save/Cancel закрывает сессию и toolkit.

Артефакты и installation receipt: `E:/temp/MH_native_selection_20260907/`.
