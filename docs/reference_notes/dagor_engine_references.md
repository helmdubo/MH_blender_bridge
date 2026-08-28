# Референсы DagorEngine для внешних агентов и аудиторов

Зафиксировано по запросу owner 2026-08-29. Внешние источники истины о
поведении Dagor; при расхождении с нашими конспектами приоритет у исходников.

## Внешние

- Исходники движка и инструментов:
  <https://github.com/GaijinEntertainment/DagorEngine>
- Официальная документация (в т.ч. Composits, dag4blend, Asset Viewer):
  <https://gaijinentertainment.github.io/DagorEngine/>

Ключевые пути в репозитории DagorEngine:

```text
prog/tools/dag4blend/                       Blender-аддон (импорт/экспорт .dag, cmp)
prog/tools/dag4blend/cmp/cmp_import.py      разбор .composit.blk (separate_by)
prog/tools/sceneTools/daEditorX/            daEditor, CompositEntityPool/CompositEntity
prog/tools/AssetViewer/                     Composite Editor (source tree, dataBlockId)
```

## Локальные (в этом репозитории)

- `reference/dag4blend/` — read-only снапшот dag4blend 2.12.0 (сток).
- `reference/dag4blend_patches/cmp_import.py` — установленный у owner
  патченный импортер (зеркала `weight:r`/`type:t`, явный
  `type:t="random"`); см. README рядом.
- `docs/03_dag4blend_analysis.md` — разбор dag4blend.
- `docs/reference_notes/dagor_corpus_inventory_20260828.md` — инвентаризация
  корпуса 26 089 `.composit.blk`.
- `docs/reference_notes/evidence/dag4blend_bridge_20260828/` — измеренные
  пробы настоящего dag4blend/gameObj/FBX.
