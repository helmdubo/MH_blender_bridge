# AMENDMENT — node hierarchy and organizational empties in `*.mesh.fbx`

Статус: **ACTIVE (r2)**. Дополняет Combined-LOD v2 profile и `05`. r1 этого
документа предлагал fail-closed запрет групп (`MH_E_UNTRANSPORTED_GROUP_NODE`);
owner решил `UE-QUESTION-19` иначе — **полная иерархия транспортируется**.
Запретный гвард r1 superseded и не реализуется. Семантика существующих
payload'ов не пересматривается: их геометрические хэши действительны.

## 1. Установленный факт (field finding, 2026-08)

Производственный дамп `sovmod_garage_shell_a_type_a.mesh.fbx` показал три
mesh node с `parent_index: -1` и пустыми `child_indices`, тогда как в Blender
сцене два из трёх мешей были детьми Empty `random`.

Разбор показал:

- **`MHFbxDump` не виноват.** Обход в `MHFbxDump.cpp` рекурсивен от
  `Scene->GetRootNode()`, пишет каждый узел, включая `eNull` («null» есть в
  таблице типов), и честно заполняет `parent_index`/`child_indices`.
  Дамп — ground truth содержимого файла.
- **Пустышки нет в самом FBX.** `mh4blend` экспортирует коллекцию через
  `use_selection=True`, а сборка выделения
  (`_collection_resource_objects`, `addon/mh4blend/scene/export_fbx.py`)
  включает только `MESH`-геометрию и aux `UCX_*`/`SOCKET_*`. `EMPTY` в
  `object_types` присутствует исключительно ради `SOCKET_*`. Обычная
  пустышка (`random`) не выделяется и в FBX не попадает.
- **Следствие Blender'а:** дети невыбранного родителя пересаживаются в
  корень FBX с запечённым world transform. В дампе это видно как
  `local_translation` −172.55/−171.78/−106.16 у `..._2_lod00` при нулевых
  трансформах остальных узлов.
- Штатный «Import into level» показывал полную иерархию с пустышками только
  для FBX, экспортированных generic-экспортом Blender без селекционного
  фильтра; к транспорту mh4blend это отношения не имеет.

## 2. Нормативные правила (r2, owner decision)

1. **Транспортируемое множество** узлов `*.mesh.fbx`: render mesh nodes всех
   LOD, aux `UCX_*`/`SOCKET_*`, и **все организационные `EMPTY` («group»)**
   ресурсной коллекции (включая авторенные в `.lods`-контейнере между
   уровневыми коллекциями). `ARMATURE`/кости зарезервированы будущим
   amendment'ом и пока не транспортируются.
2. **Иерархия сохраняется.** Группы экспортируются обычными null Model
   nodes; parenting детей не срезается, transforms не запекаются. Groups —
   структура, не LOD payload: они не несут `mh_lod_level` (stale-значения
   срезаются на экспорт) и не являются Carrier B (passport остаётся на
   каждом MESH Model, `05` §4.2 и consensus-проверка `copy_count`
   неизменны). Node identity (`mh_uid`) присваивается и группам.
3. **Замыкание по родителям.** Родитель каждого транспортируемого объекта
   обязан быть транспортируемым или `None`; иначе Blender молча
   перекорневил бы ребёнка с запечённым world transform. Экспорт
   завершается fail-closed:

   ```text
   MH_E_PARENT_OUTSIDE_RESOURCE: '<child>' is parented to '<parent>',
   which is not part of resource collection '<collection>'
   ```

4. **Semantic hash не меняется.** Durable stream `mh.meshser:2` по-прежнему
   состоит из mesh-записей в world space (`matrix_world`) и aux; группы
   участвуют только через влияние на world-матрицы детей. Появление
   иерархии в FBX без перемещения мешей — raw-изменение
   (`REPACK`/`NO_CHANGE` классы), не геометрическое.
5. **UE mapper** обязан использовать evaluated **global** transforms mesh
   nodes (мировое положение мешей идентично при плоском и иерархическом
   FBX). Null group nodes — признанная структура: не ошибка и не warning;
   они входят в node table provenance (ADR v3 §8) c `mh_role=group`.
6. **`MHFbxDump` остаётся арбитром** содержимого файла. ROADMAP: гистограмма
   attribute-типов в summary.
7. **Random/variant механики внутри mesh FBX не существует.** Групповые
   пустышки не несут селекторной семантики; вариантные наборы — уровень
   `.composite` (отдельный amendment, если потребуется).

## 3. Duplicate node UID repair (owner requirement)

Blender-дублирование (`Shift+D`/`Alt+D`) копирует custom properties, поэтому
свежий дубликат легитимно совпадает по `mh_uid` с оригиналом. Отказ экспорта
в этой ситуации — дефект UX, а не защита.

Нормативно:

1. Node-UID коллизии среди транспортируемых **объектов** и среди уникальных
   (по указателю) **mesh datablocks** чинятся на экспорт детерминированно:
   владельцы сортируются по имени datablock'а; первый сохраняет UID,
   остальные получают свежие UUID4 (персистентно, через lazy-assignment §4).
   Каждая замена логируется:

   ```text
   MH_W_NODE_UID_REASSIGNED [old_uid, new_uid]:
   duplicate node uid kept by '<keeper>'; '<renamed>' received <new_uid>
   ```

2. Linked-дубликаты (`Alt+D`) делят один datablock — это не коллизия
   datablock-UID (дедуп по указателю); чинится только object-UID.
3. Repair — resource-internal операция: цена неверного выбора keeper'а —
   один честный rebuild ресурса (`UID repair может дать честный rewrite`,
   закрытый вопрос v1). Поэтому арбитраж по manifest'у (§4.1) не требуется.
4. **Material UID не чинится никогда**: это project-global identity
   (library). Коллизия материалов остаётся fail-closed
   `MH_E_DUPLICATE_RESOURCE_UID`.
5. Пост-repair коллизия (патологический carrier) — прежний fail-closed
   `MH_E_DUPLICATE_NODE_UID`.

## 4. UE-QUESTION-19 — РЕШЕНО OWNER

Полная иерархия (группы + меши) транспортируется; вариант B r1 (запрет групп,
семантики в `.composite`) отклонён; random-механика в FBX не нужна (имя
`random` в field-кейсе было тестовым). Кости — возможное будущее расширение
транспорта, зарезервировано. Реализация r2 — `addon/mh4blend/scene/
export_fbx.py` (gather groups, parent closure, UID repair) + gates в
`tests/test_export_fbx_bpy.py`.
