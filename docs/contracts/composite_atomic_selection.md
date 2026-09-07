# Composite atomic secondary selection — 2026-09-07

Статус: READY FOR FIELD TEST. Owner подтвердил native selection/inline
Outliner и уточнил: secondary selection выбирает отдельный объект, а Edit
открывает содержащий его composite. Трансформы узлов — только после Edit.
Ветка `codex/composite-atomic-selection`, база `d18e34b`.

1. Root/secondary переключение двойным LMB сохраняется.
2. В secondary состоянии подсвечивается только точный resident leaf NodePath;
   остальные mesh instances того же occurrence и shared pool не выделены.
3. Single LMB/RMB выбирает другой объект, в том числе внутри того же occurrence.
4. Edit target определяется прежним ближайшим содержащим composite occurrence,
   внутри Edit выбран authored node этого leaf. Прямой root leaf открывает root.
5. Edit Mode, inline Outliner и быстрый drag сохраняют прежнюю семантику.

Ключ визуального выбора — SelectedPlacementLeafPath, ключ контекста Edit —
SelectedPlacementOccurrencePath. Они не подменяют друг друга. Пустой leaf
означает root selection, а пустой occurrence при непустом leaf означает
выбранный отдельный объект корневого определения.

Файлы: MHCompositeActor.h/cpp, MHCompositeNativeSelectionTest.cpp,
MHCompositeSelectionAdapterTest.cpp, docs/16_recipe_model.md §2.8, status,
этот контракт и receipt. Source, resolver, pool storage не меняются.
Red-first на atomic highlight, полный Mimir/golden, RHI smoke, три сборки,
before/after PerfTrace, docs/diff check, verified package/install с backup.
