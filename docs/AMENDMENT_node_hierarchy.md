# AMENDMENT — node hierarchy and organizational empties in `*.mesh.fbx`

Статус: **ACTIVE, fail-closed**. Дополняет Combined-LOD v2 profile и `05`;
байты существующих payload'ов не меняет. Открытая семантика групп вынесена в
`UE-QUESTION-19`.

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

## 2. Нормативные правила (сейчас)

1. Транспортируемое множество узлов `*.mesh.fbx` фиксируется явно:
   render mesh nodes всех LOD, `UCX_*`, `SOCKET_*`. Прочие объекты
   коллекции в transport **не входят**.
2. Тихое выбрасывание группирующего узла с перекорневлением детей —
   дефект контракта, а не поведение по умолчанию. Blender writer обязан
   (следующий addon slice) при экспорте обнаруживать не-aux `EMPTY` среди
   членов ресурсной коллекции и завершаться fail-closed:

   ```text
   MH_E_UNTRANSPORTED_GROUP_NODE: empty '<name>' in '<collection>' is not
   part of the mesh transport; flatten it intentionally or move the group
   semantics to a .composite (UE-QUESTION-19)
   ```

   До реализации гварда авторам предписано не держать организационные
   пустышки внутри `.lodNN`-коллекций; проверка — `MHFbxDump`
   (`summary.node_count == summary.mesh_node_count` + aux).
3. `MHFbxDump` остаётся арбитром: расхождение «Blender-иерархия ↔ дамп»
   трактуется как факт о writer'е, не о дампе. ROADMAP: гистограмма
   attribute-типов в summary, чтобы не-mesh классы были видны одной строкой.
4. Baked-transform последствия текущего поведения признаются honest debt:
   существующие payload'ы валидны, их `geometry_hash` не пересматривается.

## 3. UE-QUESTION-19 — семантика организационных/групповых узлов

**Контекст.** Авторские сцены (dag4blend-наследие) содержат группирующие
Empty (пример: `random` — в Dagor это селектор случайного варианта).
Combined-LOD профиль определяет ресурс как ОДИН static mesh; molчаливое
слияние всех детей группы в LOD0 может физически склеить взаимоисключающие
варианты. Research-модель (ADR v3 §7–8) хранит `ParentNodeUid` и
предполагает узловую иерархию в provenance.

**Вопрос.** Что означает группа внутри mesh-ресурса:

- **A.** Транспортировать пустышки как узлы `mh_role=group` (null nodes,
  иерархия сохраняется в FBX и provenance; UE не создаёт из них ассетов);
- **B.** Запретить группы в mesh-ресурсе; групповые/вариантные семантики
  (random-селекторы, switch-наборы) принадлежат `.composite`, где каждый
  вариант — отдельный ресурс/узел графа.

**Рекомендация.** **B** для `*.mesh.fbx` (сохраняет инвариант «один UID —
один StaticMesh», не размывает Combined-LOD), плюс расширение схемы
`.composite` узлом-селектором отдельным amendment. **A** остаётся fallback,
если field-практика покажет массовые организационные пустышки без
вариантной семантики.

**Временное правило.** Fail-closed §2.2: экспорт с не-aux пустышкой внутри
ресурсной коллекции блокируется; выбор A/B не предрешается.

**Статус.** ОТКРЫТ; временное правило ACTIVE.
