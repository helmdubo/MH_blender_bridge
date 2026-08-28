# dag4blend_patches — owner-патчи установленного dag4blend

Здесь лежат файлы установленного у owner dag4blend 2.12.0, отличающиеся от
стокового снапшота `reference/dag4blend/` (тот остаётся нетронутым
read-only референсом). Лицензия исходников — Gaijin; код не копировать
дословно в `addon/mh4blend`.

## cmp_import.py

Добавлен owner 2026-08-29. Отличия от стока
(`diff --strip-trailing-cr reference/dag4blend/cmp/cmp_import.py cmp_import.py`)
— ровно три правки в `build_nodes`:

1. `weight:r` каждого `ent{}` зеркалится из `dagorprops` в Blender
   ID custom properties того же Empty — внешние инструменты читают веса
   без знания RNA dag4blend;
2. random-узел (node с `ent{}`-вариантами) получает явный
   `type:t="random"` в `dagorprops` и в ID properties — сток выражал
   random-семантику только через helper-коллекцию;
3. `type:t` обычных узлов и отдельных вариантов зеркалится в ID properties.

Именно на эти зеркала опирается прямой dag4blend-адаптер mh4blend
(V5-S6.1, `docs/13_v5_s6_1_dag4blend_bridge.md`). `place_type:i`,
`placeOnCollision:b`, `ignoreParentInstSeed:b` патч не зеркалит — они
доступны через сохранённый `dagorprops` и читаются адаптером напрямую.

Известная особенность стока (сохранена и в патче): в `apply_matrix`
ветка `offset_y:p2` продублирована, поэтому `offset_z:p2` не применяется
к позиции Empty визуально (в `dagorprops` значение сохраняется; на
MH-маршрут не влияет — inline p2 заблокирован OPEN-V5-15).
