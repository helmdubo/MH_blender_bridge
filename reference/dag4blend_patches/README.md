# dag4blend_patches — owner-патчи установленного dag4blend

Здесь лежат файлы установленного у owner dag4blend 2.12.0, отличающиеся от
стокового снапшота `reference/dag4blend/` (тот остаётся нетронутым
read-only референсом). Лицензия исходников — Gaijin; код не копировать
дословно в `addon/mh4blend`.

Соответствие файлов:

| файл здесь | путь в dag4blend | патч |
| --- | --- | --- |
| `cmp_import.py` | `cmp/cmp_import.py` | №1 (weight/type зеркала) |
| `importer.py` | `importer/importer.py` | №2 (collision/phmat зеркала) |

`backup_installed_20260830/` — копии заменённых файлов установленного
dag4blend, снятые непосредственно перед установкой патча №2.

## cmp_import.py — патч №1

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

## importer.py — патч №2 (collision / phmat)

Добавлен 2026-08-29 по `docs/15_v5_s6_1_1_hardening.md` §3 (носители —
§3.4). Отличия от стока
(`diff -u --strip-trailing-cr reference/dag4blend/importer/importer.py importer.py`)
— ровно две правки, обе в DAG-импортёре:

1. модульная константа `COLLISION_PROPS_MIRRORED` (после `BEENTHERE = 2`) —
   список зеркалируемых ключей;
2. хвост `DagImporter.buildObjProperties` — после разбора script-блока
   ноды те же ключи пишутся в Blender ID custom properties того же
   mesh-объекта.

```diff
+#Dagor collision node script keys mirrored into Blender ID custom properties
+COLLISION_PROPS_MIRRORED = ('collision:t', 'phmat:t', 'isPhysCollidable:b', 'isTraceable:b')
...
         if broken_properties.__len__() > 0:
             dagorprops['broken_properties:t'] = ";".join(broken_properties)
+# Dagor keeps collision and phys material data in the node script block, which
+# the stock importer stores in dagorprops only. Mirror those keys onto the same
+# mesh object as Blender ID custom properties, so external tools can read the
+# collision setup without knowing the dag4blend RNA. ':t' values lose the DAG
+# quoting and become bare tokens, ':b' values stay real booleans. dagorprops is
+# left untouched, so export keeps writing the original script block.
+        for name_type in COLLISION_PROPS_MIRRORED:
+            value = dagorprops.get(name_type)
+            if value is None:
+                continue
+            if name_type.endswith(':b'):
+                o[name_type] = bool(value)
+            else:
+                o[name_type] = f'{value}'.strip('"')
         return
```

### Где данные лежат на самом деле

Owner-спека говорит «в отдельном текстовом файле» — это UI-представление
(оператор `dt.props_to_text` выгружает свойства в текст-датаблок
`props_temp`). Фактическая цепочка:

`DAG_NODE_SCRIPT`-чанк ноды → `node.objProps` (сырая строка) →
`DagImporter.buildObjProperties` → `obj.dagorprops['<name>:<type>']`
(`PropertyGroup` на `bpy.types.Object`, т.е. вложенная IDProperty-группа
`dagorprops`, а не top-level custom property объекта). Отдельного файла
нет; до патча внешний инструмент видел бы только `obj["dagorprops"]`.

### Значения

`buildObjProperties` кладёт `:t` со стоковыми кавычками из DAG
(`phmat:t` → `"wood"`), `:b` — уже настоящим `bool`. Зеркало снимает
кавычки: ID prop `phmat:t` = `wood` (токен реестра
`docs/reference_notes/dagor_phmat_registry.md`), `collision:t` = `mesh` /
`box` / `convex` / `capsule`; `isPhysCollidable:b` / `isTraceable:b` —
`True`/`False`. `dagorprops` не трогается вообще, поэтому экспорт
dag4blend пишет тот же script-блок, что и раньше.

Зеркалятся только четыре ключа и только когда они есть в ноде.
`renderable:b` / `collidable:b` не зеркалятся (вне scope, §4). Ключи
`collision:t`/`phmat:t` встречаются и на render-нодах
(`renderable:b=yes, collidable:b=no`, например `wires_a.*` в
laundry-даге) — они тоже получают зеркало; классификация узла коллизии
остаётся material/name-first из S6.1.1, свойства только обогащают её.

### Как проверялся

Изолированная копия dag4blend (свой `BLENDER_USER_RESOURCES`,
`addon_utils.enable`, перенаправленные `show_popup`/`show_text`),
Blender 4.5.12 LTS `--background --factory-startup`, реальные DAG из CDK:

- `.../mall_omsky_a/composit_parts/laundry/sovmod_mall_omsky_laundry_a.lod00.dag`
  (узлы `laundry_a_wood_cls_phys.001`, `laundry_a_wood_cls_trace.001` из
  owner-спеки, плюс steel/ceramic/concrete),
- `.../sovmod_mall_omsky_laundry_bench_a.lod00.dag` (`collision:t="box"`).

Импорт `bpy.ops.import_scene.dag` прогонялся до и после патча, снимался
JSON-снапшот сцены (объекты: имя/тип/родитель/коллекции/ID props/
dagorprops/слоты материалов/число вершин/матрица; коллекции; материалы с
`dagormat`; сцены; текст-датаблоки). Дифф: 162 объекта до и после,
изменилось единственное поле `id_properties` у 14 объектов — ровно
добавление зеркал (8 узлов `*_cls_*` получили все четыре ключа,
6 render-узлов `wires_a.*` — только `collision:t`/`phmat:t`, которые у них
и есть в ноде). `dagorprops`, геометрия, материалы, коллекции, сцены и
тексты идентичны байт-в-байт. Артефакты прогона (снапшоты
`snapshot_before.json` / `snapshot_after.json`, probe-скрипт) — рабочий
каталог `E:\MimirComposite_V5S6_1_2_PhmatPatch_20260829`, вне репозитория.

Полевая особенность реального контента: в laundry-даге встречается
`phmat:t="cermic"` (опечатка художника, нет в реестре). Патч переносит
токен как есть — нормализация и предупреждение об отсутствующем
`UPhysicalMaterial` относятся к UE-стороне S6.1.2.
