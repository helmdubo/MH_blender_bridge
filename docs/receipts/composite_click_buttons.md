# Composite LMB / RMB — execution receipt, 2026-09-07

История исполнения, не норматив. Ветка `codex/composite-click-buttons`, база
`873a7d1`; контракт `docs/contracts/composite_click_buttons.md`.
Owner подтвердил drag предыдущего среза и запросил разделение кнопок.

## Реализация и границы

Обычный pooled hit возвращает native owner actor и очищает логический leaf:
LMB выбирает/подсвечивает всё размещение. Первичный hit оставляет одноразовый
weak-owner/path/frame токен. Только синхронно открываемое Viewport RMB меню
с совпадающим resolved owner применяет его для подсветки вложения. Double-click
и FromSecondary токен не создают; просроченный, чужой и уже использованный
токен не применяется. Активный Edit и selection lock защищены от этого пути.

Проверено в UE 5.7 source: `ViewportSelectionUtilities.cpp` использует Primary
для LMB и RMB, Secondary для double-click; передаёт в SummonContextMenu уже
resolved actor handle. `LevelEditorContextMenu.cpp` сохраняет его в контекст.
Поэтому самостоятельный lookup raw ISM по menu HitProxyElement невозможен.
Input preprocessor не добавлялся; RMB navigation и keyboard modifiers остаются
в штатном click handler. Редактирование/математика/drag не менялись.

Сопоставление с [официальными controls UE5.7](https://dev.epicgames.com/documentation/en-us/unreal-engine/viewport-controls-in-unreal-engine?application_version=5.7):
LMB select и RMB hold для камеры сохранены; выбор вложения через RMB —
расширение контекстной команды плагина, не штатный универсальный UE selection.

## Проверки

Все логи в `E:/temp/MH_CEI1_host_20260907/`. Собственные host и strict host
из предыдущего среза, UE5.7.4 CL51494982, BuildId 47537391.

`CLICK_RED_BUILD.log` SUCCESS; `CLICK_RED.log` — оба selection-теста RED:
прежний код оставлял внутренний leaf при Primary/Secondary и не возвращал
подсветку всего owner после LMB. Прежние ожидания Primary→leaf изменены по
прямому уточнению owner; тесты не удалялись.

Green coverage: реальный raw-hit → Primary owner resolution → context helper;
LMB после RMB, explicit siblings и Random composite option, one-shot,
wrong-owner/foreign/stale handle, отсутствие токена для double-click,
Edit Contents с preselected authored node, obsolete path после rebuild.

Финальная версия:

- Guarded non-unity/no-PCH: `CLICK_GUARDED.log` SUCCESS.
- StrictIncludes, non-unity/no-PCH: `CLICK_STRICT_FINAL.log` SUCCESS.
- Force-unity/adaptive-off: `CLICK_UNITY.log` SUCCESS.
- Полный Mimir/golden NullRHI: `CLICK_FULL.log`, `ClickFullReport` — 291/291,
  0 failed, включая оба RecipeShadowParity.
- D3D12 offscreen smoke на strict DLL: `CLICK_RHI.log`, `ClickRHIReport` —
  63/63, 0 failed (EditMode, Selection, Async, persisted dependencies,
  save proof и RecipeShadowParity). Это automation, не ручной viewport тест.
- PerfTrace before/after: `CLICK_BEFORE.log` / `CLICK_AFTER.log`, по 2/2.
  Mapload 0.131 → 0.131 ms; sync package loads, registry lookups, identity
  admissions и proof build — 0; reused components 2, created 0.
  Targeted reimport сохраняет full scans 0 и notified actors 1;
  total 130.576/116.601 → 142.523/127.576 ms. After выполнялся одновременно
  с RHI smoke; одиночные timings не являются сравнительным бенчмарком.
- Edit drag fixture: 100 updates 12.46 ms (0.125 ms/update), 5 components;
  50 enter/exit cycles 217.77 ms. Это небольшой тестовый fixture, не cottage.
- `tools/check_normative_docs.py` и `git diff --check` — PASS.

Результат: READY FOR FIELD TEST. Source/DLL соответствие и установка
фиксируются в `manifest.json` и `installation.json` каталога пакета ниже.

## Полевой сценарий

1. LMB на гараж: выбран весь cottage, штатное перемещение parent actor.
2. RMB на меш гаража: подсвечивается гаражный подкомпозит, через Composite
   Options → Edit Contents открывается он, нужный внутренний узел уже выбран.
3. Закрыть Edit; RMB на другое вложение, затем LMB: снова выбран весь cottage.
4. Удерживать RMB и перемещать камеру: режим выбора/редактирования не меняется.

Пакет и installation receipt: `E:/temp/MH_click_buttons_20260907/`.
