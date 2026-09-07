# Composite viewport: LMB / RMB — 2026-09-07

Статус: READY FOR FIELD TEST. Owner подтвердил качество драга и уточнил
normal viewport interaction. Ветка `codex/composite-click-buttons`, база
`873a7d1`; продолжение `composite_drag_selection.md`.

Вне Edit LMB выбирает native owner actor и подсвечивает всё размещение.
RMB, открывающий штатное контекстное меню, выбирает содержащее задетый mesh
вхождение композита; Edit Contents открывает его с выбранным authored node.
RMB с удержанием для навигации камеры не меняет логический выбор. Повторный
LMB после RMB сбрасывает внутренний контекст, даже если owner уже выбран.
Edit Mode и быстрый drag сохраняют нынешнее поведение.

`ETypedElementSelectionMethod::Secondary` означает double-click, не RMB.
Контекст получает уже разрешённый owner в `HitProxyElement`. Точный leaf
передаётся из selection adapter одноразовым weak-owner/path/frame токеном:
его может потребить только синхронно открываемое Viewport меню того же owner,
вне активной edit session. Токен предыдущего кадра не применяется.
World Outliner/root menu,
пустой или чужой hit не наследуют прежний выбранный leaf.

Scope: SelectionAdapter h/cpp, SourceToolMenus cpp, SelectionAdapterTest cpp,
документация. Source protocol, resolver, pool storage и edit gestures неизменны.
Red-first LMB reset и сохранение owner selection; затем RMB exact-hit helper,
повторные sibling occurrences, random option, stale/foreign/root hit и
Edit preselection. Полный Mimir/golden, оба ShadowParity, guarded non-unity,
StrictIncludes, force-unity, RHI smoke, до/после traces, docs/diff checks.
Проверенный пакет устанавливается в ранее указанный Plugins/MimirComposite
при закрытом пользовательском UE, с резервной копией предыдущей версии.
