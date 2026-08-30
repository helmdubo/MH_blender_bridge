# Квитанция: proxymat-материалы и публикация include_all

Статус: **READY — прежний STOP снят полевым дополнением 2026-08-31 (§10)**.
Дата: 2026-08-30  
Ветка: `feat/proxymat-and-publish-perf`  
База: `3ee5a2cdbca5bcf94eb3d62cc978f4470cf9eb11`  
Scope исходного checkpoint: `addon/mh4blend`, `tests`, эта квитанция.
Дополнение §10 включает совместимый reader/writer в `ue/MimirComposite`.

## 1. Итог на checkpoint

Этот раздел фиксирует историческое состояние на 2026-08-30. Текущее green-
закрытие macro textures и opaque provenance приведено в §10.

Реализованы и проверены:

- read-only parser `.proxymat.blk`, повторяющий чтение dag4blend;
- file-authority для proxy-материала при `dagormat.is_proxy == True` или
  `shader_class.endswith(":proxymat")`;
- stem-резолв обычных `texN`, file-authority fingerprint и точный отказ при
  отсутствующем proxy-файле;
- отдельное инструментирование публикации;
- full-hash admission один раз на batch, далее size/mtime-проверки и полный
  read/hash только изменившегося или текущего target;
- один parent-directory fsync на директорию после всего успешного batch;
- детект внешней модификации опубликованного prefix до следующего replace.

Не завершён один ратификационно зависимый пункт: frozen `.material.params`
принимает только scalar/vector4 и отклоняет строки. Все 37 реальных proxymat из
найденного `trees_leaf` содержат `$(ASSET_NAME)`. Parser сохраняет их отдельно
от textures, а публикация сейчас fail-closed останавливается; данные не
теряются и в texture closure не попадают. Полный CDK include_all и UE-import
provenance-кейса поэтому не объявляются пройденными.

## 2. Часть A — red-first и семантика proxymat

### Красный baseline

Blender 4.5.12, `tests/test_export_material_bpy.py`, четыре новых кейса:

```text
FAILED test_proxy_placeholder_reloads_file_and_duplicate_script_last_wins
FAILED test_proxy_shader_suffix_is_file_authority_and_not_a_class_token
FAILED test_proxy_missing_source_fails_with_full_path_and_remedy
FAILED test_two_proxy_claimants_of_one_file_have_identical_payload
4 failed, 33 passed in 2.67s
```

Пустышка молча брала stale dagormat-cache и затем падала
`MH_E_MATERIAL_GRAMMAR` на пустом class или `*:proxymat`, то есть файл не был
authority и отсутствующий файл не давал требуемого source-отказа.

### Green

После `3935682`:

- тот же Blender-модуль: **38 passed / 0 failed**;
- чистый parser: **2 passed / 0 failed**;
- scene-placeholder и сам `.proxymat.blk` остаются неизменными;
- два proxy-claimant одного логического имени/файла дают одинаковые canonical
  `.material` bytes;
- отсутствующий файл даёт `MH_E_INVALID_RESOURCE_SOURCE`, полный exact path и
  remedy `check proxy_path or run the dag4blend proxymat search`;
- обычный абсолютный чужой texture path сводится к basename stem существующим
  контрактом.

Правило дублей установлено по reference:
`dagormat_prop_add -> add_custom_prop -> try_remove_custom_prop` удаляет
предыдущее custom property перед добавлением. Поэтому повторяющийся
`script:t="key=value"` использует **последнее значение (last wins)**. Parser
читает строки по порядку и присваивает тот же ключ повторно. `power:r`, как и
в `dagormat_from_text`, не входит в сериализуемый материал.

Пробелы удаляются на стадии `read_proxy_blk` из всей строки до semantic parse.
Тип script-value определяется в порядке reference: matrix marker, bool,
vector, float, int, string; неподдерживаемые frozen-форматом типы не
ремонтируются, а блокируются существующим
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE`.

### Реальное CDK-дерево

Read-only профиль:

```text
CDK_PROXY files=37 macro_files=37 textures=154 params=563
CDK_DUPLICATE file=tree_castanea_aesculus_city_burnt_cut.proxymat.blk
is_pivoted=1
macros={'tex7': '$(ASSET_NAME)_pivot_pos.dds',
        'tex8': '$(ASSET_NAME)_pivot_dir.dds'}
```

Источник:
`H:\_Gaijin_Entertainment\EnlistedCDK\ActiveMatterCDK.2024.11.11\EnlistedCDK\develop\assets\gameproj\nature_common\entities\vegetation\trees_leaf`.

Реальный Blender read-path доходит до сохранённого macro provenance и
останавливается до записи:

```text
MH_E_MATERIAL_NOT_ROUNDTRIPPABLE:
...\tree_bark_apple_dead.proxymat.blk:
proxymat macro texture provenance requires string params before publication
(tex7='$(ASSET_NAME)_pivot_pos.dds',
 tex8='$(ASSET_NAME)_pivot_dir.dds');
macro slots were not added to textures
```

Это ожидаемый временный STOP, а не green acceptance реального include_all.

## 3. Часть B — инструментирование

Отдельный commit `a3b7263` добавил к atomic receipt длительности:

- `lock`;
- `write_fsync`;
- `read_back`;
- `guard`;
- `replace`;
- `parent_fsync`.

Guard дополнительно считает inventory scan, source/staged read bytes и число
полностью проверенных hash bytes. Метрики находятся только в operation
receipt, не сериализуются в Source Protocol payload и не меняют canonical
bytes.

Красный 100-payload synthetic на прежнем guard:

```text
MH_PUBLISH_100 wall_ms=15078.000 guard_reads=14950
assert guard_reads <= 200
E assert 14950 <= 200
1 failed, 1 passed, 14 deselected
```

`14950 = 10000` повторных staged reads + `4950` повторных reads уже
опубликованного prefix. Отдельная инъекция внешней модификации между первым и
вторым replace была поймана и до оптимизации.

## 4. Часть B — после оптимизации

Финальный прогон той же 100-payload фикстуры:

```text
MH_PUBLISH_100 wall_ms=3422.000
guard_reads=0
metadata_checks=4950
max_payload_ms=94.000
stages={
  'lock': 156.0,
  'write_fsync': 140.0,
  'read_back': 30.0,
  'guard': 2450.0,
  'replace': 127.0,
  'parent_fsync': 32.0
}
16 passed in 5.83s
```

Результат:

- повторные source/staged reads: **14950 -> 0**;
- local synthetic wall: **15.078 s -> 3.422 s**;
- относительно ратифицированной полевой базы Lead `196 s` — **1.75%** и ниже
  абсолютного acceptance `20 s`;
- малый payload: максимум **94 ms**, порог `<=100 ms` выполнен;
- metadata checks остаются: это нормативная size/mtime сверка snapshot между
  replace, а не повторное чтение/хэширование;
- fsync каждого payload сохранён; unit gate доказывает один directory-fsync
  для единственной общей parent directory в конце batch.

Staging-файл после полного admission больше не опрашивается N раз: atomic
publisher использует immutable bytes из `StagedClosurePayload`, а не заново
открывает staged path. Изменение staged path после admission не способно
изменить публикуемые bytes.

Инъекция после первого replace теперь даёт:

```text
published=('composite:bulk_000',)
cause=MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED
second target does not exist
```

После появления необратимого prefix тот же отказ оборачивается штатным
`MH_E_PARTIAL_PUBLISH`. Crash/failure-boundary тесты до/после replace остались
зелёными.

## 5. Гейты

| Гейт | Результат |
|---|---|
| Pure `python -m pytest tests/ -q` до правок | **298 passed / 14 skipped** |
| Pure после | **302 passed / 14 skipped / 0 failed** |
| Blender 4.5.12, 12 отдельных factory-startup процессов | **328 passed / 0 failed** |
| `test_payload_publish_v2.py + test_batch_publish.py` | **25 passed / 0 failed** |
| `test_export_closure_bpy.py` | **16 passed / 0 failed** |
| `test_dag4blend_publication_bpy.py` | **38 passed / 0 failed** |
| `test_export_material_bpy.py` | **38 passed / 0 failed** |
| `git diff --check` | **PASS** |

Blender-модули: `22 + 20 + 76 + 38 + 16 + 16 + 58 + 38 + 15 + 6 +
12 + 11 = 328`.

Полный реальный `trees_leaf -> include_all -> .material` и UE importer test
для строковых provenance params: **NOT RUN / STOP** на этом историческом
checkpoint; текущее закрытие приведено в §10.

## 6. Frozen-инварианты

- `golden` tree до/после:
  `71b30ebf65ca3cc8473f50305990c2bf2b332727`;
- `reference` tree до/после:
  `12e25b76b19aa824458221cf23f77236a17382cd`;
- `addon/mh4blend/core/canonical.py` blob до/после:
  `bbe340ce0f7c46d50e097ac1b7f8ea0b831dd945`;
- новых E/W-кодов нет;
- canonical payload writers, golden и reference не менялись;
- Blender snapshot-гейты зелёные; proxy-read не заполняет dagormat и не
  мутирует сцену.

## 7. Вопросы

Исторический вопрос ниже разрешён реализацией §10: macro slots стали
конкретными per-mesh texture bindings, а остальные string/bool script values
сохраняются как opaque provenance.

1. **Контекст:** `.material.params` frozen v4 принимает только number/vector4;
   строка отклоняется. Все 37 реальных `trees_leaf` proxymat содержат macro
   texture slots, а owner требует сохранить их как
   `params["proxymat_texN_macro"] = "$(ASSET_NAME)..."`. UE-side importer test
   также лежит вне заданного общего scope `addon/mh4blend + tests`.
   **Вопрос:** ратифицировать ли минимальное расширение params-грамматики —
   string разрешена только для ключей `proxymat_tex0_macro` ...
   `proxymat_tex15_macro` — и точечный UE importer Automation-тест в
   `ue/MimirComposite`?
   **Временное fail-closed допущение:** macro slots распознаются и сохраняются
   parser-моделью, не входят в `textures`/texture closure, но `.material` с
   ними не публикуется до решения; реальный CDK acceptance остаётся STOP.

## 8. Изменённые файлы

- `addon/mh4blend/core/batch_publish.py`;
- `addon/mh4blend/core/payload_publish_v2.py`;
- `addon/mh4blend/core/proxymat.py`;
- `addon/mh4blend/scene/export_closure.py`;
- `addon/mh4blend/scene/export_material.py`;
- `tests/test_batch_publish.py`;
- `tests/test_export_closure_bpy.py`;
- `tests/test_export_material_bpy.py`;
- `tests/test_payload_publish_v2.py`;
- `tests/test_proxymat.py`;
- `docs/receipts/proxymat_publish_perf.md`.

## 9. Коммиты checkpoint

- `a3b7263` — `Instrument payload publication stages`;
- `3935682` — `Read proxymat materials from authoritative sources`;
- `d72dc6d` — `Reuse batch inventory validation during publication`;
- квитанция — отдельный documentation commit.

## 10. Дополнение 2026-08-31 — macro textures и opaque provenance

### 10.1. Разрешение `$(ASSET_NAME)`

Полевой пример `bush_beech_bark.proxymat.blk` использует:

```text
tex7=$(ASSET_NAME)_pivot_pos.dds
tex8=$(ASSET_NAME)_pivot_dir.dds
```

Один bark proxymat применяется к семи разным mesh resources
(`bush_beech_medium_a/b/c`, `bush_beech_small_a/b/c/d`), и у каждого на диске
своя пара pivot DDS. Поэтому material-level подстановка без mesh-контекста
была бы неоднозначной. Реализовано точное разрешение на границе mesh export:

- asset name берётся из managed mesh stamp либо из `.lodNN`/`.lods`
  collection;
- macro разворачивается в реальный `tex7/tex8` logical texture token;
- общий proxymat специализируется как `<material>__<mesh>`, например
  `bush_beech_bark__bush_beech_medium_a`;
- FBX material binding и `.material` используют одно имя;
- если точного mesh authority нет, операция остаётся fail-closed;
- сцена, dagormat и `.proxymat.blk` не мутируются.

`Copy All Textures to Project` теперь проходит тем же per-mesh разрешением и
копирует сами pivot DDS. Повторный Copy All после Remap является проверенным
no-op: внешний CDK path и уже project-local path объединяются только при
равных size + SHA-256; разные байты по-прежнему дают
`MH_E_AMBIGUOUS_RESOURCE_NAME`.

### 10.2. Script provenance

Реальный preflight после macro-разрешения обнаружил допустимые Dagor script
values `lighting=vltmap` и `real_two_sided=no`. Чтобы выполнить требование
«script params как есть», `.material.params` получил append-only поддержку
string/bool:

- Blender canonical reader/writer и UI сохраняют number/vector4/string/bool;
- UE reader/writer принимает те же canonical values;
- string/bool являются opaque provenance, не подделываются под scalar/vector
  MI parameters;
- `SourceHash` покрывает полный документ, `AppliedHash` — только реально
  материализуемое подмножество;
- UE Publish managed материала перечитывает существующий source и сохраняет
  opaque provenance.

Новых E/W-кодов, версии importer и изменений golden/reference нет.

### 10.3. Red -> green

Красный полевой отказ:

```text
MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: ...bush_beech_bark.proxymat.blk:
proxymat macro texture provenance requires string params before publication
(tex7='$(ASSET_NAME)_pivot_pos.dds',
 tex8='$(ASSET_NAME)_pivot_dir.dds');
macro slots were not added to textures
```

Green на сохранённой сцене
`E:\portfolio\sovmod_cottage_i_cmp_source_lod00.blend`:

```text
materials=519
referenced_slots=1254
macro_slots=84
macro_assets=21
physical_sources=653
```

Фактический автоматический Copy All в настроенный source root
`E:\blender_plugin`:

```text
unique_files=647
copied=60
skipped_identical=587
```

Разница `653 -> 647` — шесть одинаковых источников, сходящихся в один
project destination и подтверждённых по bytes. Для
`bush_beech_medium_a.lods` реальный `prepare_fbx_collection` прошёл и выдал:

```text
bush_beech_bark__bush_beech_medium_a:
  tex0=ground_plant_beech_bark_tex_d
  tex2=ground_plant_beech_bark_tex_n
  tex7=bush_beech_medium_a_pivot_pos
  tex8=bush_beech_medium_a_pivot_dir
```

### 10.4. Гейты дополнения

| Гейт | Результат |
|---|---|
| Pure `python -m pytest tests/ -q` | **308 passed / 14 skipped / 0 failed** |
| Blender 4.5.12, 12 отдельных factory-startup процессов | **352 passed / 0 failed** |
| Guarded UE editor build (`-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH`) | **Succeeded** |
| `Mimir.V4.Material.StringProvenance` | **1/1 success** |
| Полный NullRHI `Mimir` с `-MHGoldenRoot` | **161/161, 0 failed, 0 not run** |
| `git diff --check` | **PASS** |

NullRHI breakdown: **119 success + 42 success-with-warnings**. Первый общий
Blender-запуск был отброшен как environmental: cp313 `xxhash` был ошибочно
выше bundled cp311 wheel в `sys.path`; повтор с ABI Blender 4.5 прошёл 352/0.

Frozen tree hashes остались:

- `golden`: `71b30ebf65ca3cc8473f50305990c2bf2b332727`;
- `reference`: `12e25b76b19aa824458221cf23f77236a17382cd`;
- `canonical.py`: `bbe340ce0f7c46d50e097ac1b7f8ea0b831dd945`.

Код дополнения: `0e8fccf` —
`Resolve Dagor proxy textures per mesh asset`.

### 10.5. Полевое дополнение — Dagor parameter case

Следующий реальный blocker был внешним camelCase key:

```text
MH_E_MATERIAL_GRAMMAR: material 'glass.001' / params key:
value 'isShell' must match [a-z0-9_]+ exactly
```

Строгая MH-грамматика не расширена. На Dagor adapter boundary ключи
`[A-Za-z0-9_]+` детерминированно переводятся в lowercase (`isShell` ->
`isshell`). Пунктуация/Unicode не ремонтируются. Пара `isShell` + `isshell`
fail-closed блокируется до публикации, а не перетирает одно значение другим.

Red-first: новый Blender-тест воспроизвёл исходный
`MH_E_MATERIAL_GRAMMAR`; после проекции focused gate дал **2 passed**, полный
`test_export_material_bpy.py` — **47 passed**, все 12 Blender-hosted модулей —
**354 passed / 0 failed**. Read-only проверка сохранённой сцены дала:

```text
MH_FIELD_OK glass_de3ff22636b9
  isshell=1
  max_thickness=0.01
  min_thickness=0.001
```

Pure suite остался **308 passed / 14 skipped**.

### 10.6. Полевое дополнение — Blender ID-name limit

Per-mesh proxymat specialization выявила лимит Blender material ID в 63 bytes:

```text
MH_E_AMBIGUOUS_RESOURCE_NAME:
Blender could not assign the transport material name
'ground_plant_causonis_japonica_bark__ground_plant_causonis_japonica_b'
```

Красный тест зафиксировал derived name длиной **69 bytes**. Теперь имя до 63
bytes остаётся прежним, а длинное получает 50-байтовый читаемый prefix + `_` +
12 hex SHA-256 полного имени. Разные полные имена не сливаются; повторный
resolve детерминирован. Реальный read-only stage сохранённой сцены прошёл:

```text
ground_plant_causonis_japonica_bark__ground_plant__8a802214a69a  63 bytes
ground_plant_causonis_japonica_branch__ground_plan_aa76cfa79d0b 63 bytes
MH_STAGE_OK ground_plant_causonis_japonica_b 46668 bytes
```

Focused red -> green: **1 failed -> 1 passed**; полный
`test_export_material_bpy.py`: **48 passed**; pure suite: **308 passed / 14
skipped**; все 12 Blender-hosted модулей: **355 passed / 0 failed**.

Prefab/collision diagnostics из того же полевого лога не маскируются:

- collision nodes без однозначного `phys` либо `trace`, а также комбинация
  `isPhysCollidable=True + isTraceable=True`, остаются warning и не входят в
  payload до отдельного решения о UE-семантике;
- prefab публикуется как mesh только при явном per-run
  `Allow Prefab as Mesh (Lossy)`; default остаётся fail-closed;
- warning «exported as mesh by explicit policy» и error «requires explicit»
  не могут возникнуть в одном вызове: это строки разных запусков, оставшиеся
  вместе в Blender report history.

### 10.7. Полевое дополнение — длинный LOD node

Следующий blocker возник при временном добавлении обязательного `_lod00`:

```text
MH_E_INVALID_LOD_HIERARCHY:
Blender could not assign temporary LOD node name
'sovmod_tropospheric_station_building_gate_a_jamb_origin001_lod00'
```

Красный тест воспроизвёл обрезание Blender. Для derived LOD transport name
теперь используется UTF-8-safe prefix + `_` + 12 hex SHA-256 + неизменный
terminal `_lodNN`, всё имя не длиннее 63 bytes. Авторское Blender-имя после
FBX writer восстанавливается. Реальный stage проблемного ресурса прошёл:

```text
MH_STAGE_OK sovmod_cottage_i_garage_gate_jamb_a 52684 bytes
sovmod_tropospheric_station_building_gate_a__5347490fdc85_lod00 63 bytes
```

Парсер классифицировал узел как LOD0. Red -> green: **1 failed -> 1 passed**;
полный `test_export_fbx_bpy.py`: **62 passed**; pure suite:
**308 passed / 14 skipped**; все 12 Blender-hosted модулей:
**356 passed / 0 failed**.
