# 04 — Source Workflows v1 (NORMATIVE FREEZE CANDIDATE)

Статус: нормативный workflow кандидата на финальную on-disk схему v1. Документ
заменяет bundle-oriented UX и соответствующие части документов 00–02. Точный
schema-doc commit становится FINAL v1 FREEZE после приёмки внешним ревьювером;
после этого байтово-значимые изменения требуют `schema_version = 2`.

## 1. Инвариант источников

`.composite` задаёт граф: NodeUID, target ResourceUID, parenting, transforms и
metadata. Resolver находит payload по ResourceUID. FBX только наполняет geometry
Collection и **никогда** не является источником composite nodes. Импорт
одиночного FBX не может достоверно восстановить composite hierarchy.

Материал — самостоятельный ресурс: один `mh.material` v1 ↔ один Blender
Material ↔ один будущий UE Material Instance. Он не принадлежит мешу и не
размещается автоматически рядом с первым потребителем.

## 2. Настройки проекта

### `source_root`

`source_root` — единая граница source tree проекта и resolver scope. Это не
Bundle Root и не Texture Root. Все межкаталожные ссылки ищутся только под этим
корнем; произвольного поиска по остальному диску нет.

Пути `resources[].source`, `registry.source_path` и необязательный
`registry.manifest_path` нормализуются с `/`. Manifest хранит `source`
относительно каталога owning manifest; registry хранит оба path-hint
относительно `source_root`.

### `texture_policy`

Настройка принимает:

- `transitional` — значение по умолчанию: текстура вне `source_root` даёт
  `MH_W_TEXTURE_OUTSIDE_ROOT`, операция продолжается;
- `strict` — будущий CI-режим: тот же факт даёт
  `MH_E_TEXTURE_OUTSIDE_ROOT`, операция блокируется.

Отдельного Texture Root и remap-настройки нет. Текстуры не копируются и не
перемещаются.

## 3. UX: три независимых workflow

В аддоне нет Bundle Export, Export Sources и неявного экспорта dependency
closure.

### MH FBX

- **Collection** — одна geometry definition;
- **Folder** — каталог первого экспорта;
- **Export FBX** — атомарно записывает один FBX и upsert'ит его строку в
  `export_manifest.json` этого каталога.

Слоты записывают только `material_uid`. Если соответствующий `.material` ещё
не найден, FBX всё равно экспортируется, лог получает warning со списком, а
быстрое действие **Export materials…** переводит к явному material workflow.
Материалы рядом с FBX автоматически не создаются.

### MH Composite

**Import:** `*.composite` path + **Import Composite**. Рекурсивное разрешение
дочерних composites и импорт найденных FBX всегда включены. Галок `Recursive`
и `With dags/FBX` в v1 нет. Возможный будущий `Structure only` — отдельный
режим из ROADMAP, не комбинация галок.

**Export:** Collection + Folder + **Export Composite**. Экспортируется ровно
одна definition. Дочерние ресурсы не экспортируются автоматически.

**Resolve Missing** повторяет resolver cascade для всех unresolved-узлов сцены
и рекурсивно наполняет уже существующие target Collections, включая только что
найденные child-composite definitions. Существующие узлы не пересоздаются,
NodeUID/ResourceUID не меняются.

### MH Material

- **Material** — один Blender Material;
- **Folder** — выбирается только для первого экспорта UID;
- **Export Material** — пишет один `.material` и upsert'ит resource-row owning
  manifest.

После первого экспорта resolver находит единственного владельца UID, и
последующие экспорты обновляют существующий файл **in place**. Новое значение
Folder не переносит ресурс и не создаёт второго владельца.

Rename материала обновляет `name` в payload и manifest, но не переименовывает
существующий файл. Соответствие `<sanitized_name>` текущему `name` не требуется
и не проверяется: identity задаёт полный UID, а `uid8` является обязательным
filename-disambiguator. Автоматический rename запрещён;
ручной транзакционный **Rename file to match** — только ROADMAP.

## 4. Manifest — опись собственных ресурсов

Каждый каталог с source payload может содержать `export_manifest.json`:

```json
{
  "schema": "mh.export_manifest",
  "schema_version": 1,
  "exporter_version": "0.4.0",
  "resources": [
    {
      "uid": "2db5574c-3aca-43cc-9ab5-8242403e18cd",
      "kind": "static_mesh",
      "name": "wall_a",
      "source": "wall_a__2db5574c.mesh.fbx",
      "content_hash": "xxh3:9f2c01ab34cd56ef",
      "material_slots": [
        {
          "slot_name": "m_metal",
          "material_uid": "7d99d3ac-95ba-44d0-91d5-b95f8a9fce90"
        }
      ]
    },
    {
      "uid": "7d99d3ac-95ba-44d0-91d5-b95f8a9fce90",
      "kind": "material",
      "name": "m_metal",
      "source": "m_metal__7d99d3ac.material",
      "content_hash": "xxh3:67c9db59a8b33f0d"
    },
    {
      "uid": "f53d93af-1a52-4f5e-bb8c-a2b1de796d2c",
      "kind": "composite",
      "name": "window_set_a",
      "source": "window_set_a__f53d93af.composite",
      "content_hash": "xxh3:aa010fa05a09dabc"
    }
  ]
}
```

Manifest — квитанция и опись payload, которыми владеет его каталог. В нём нет
`materials[]`, `external_dependencies`, bundle identity и snapshot-семантики.
Зависимости вычисляются из `resource_uid` в `.composite` и `material_uid` в
mesh `material_slots`; их денормализованная копия не хранится.

Для каждого resource UID допустим ровно один owning manifest под
`source_root`. `source` относителен owning manifest и указывает на payload того
же UID/kind. Честный результат resolver содержит всю тройку:

```text
uid -> { payload_path, owning_manifest_path, manifest_row }
```

Именно manifest-row несёт `content_hash`, `material_slots` и будущие поля вроде
`lods[]`; наличие одного payload-файла не завершает resolve.

Standalone writer делает UID-upsert и сохраняет несвязанные строки и файлы.
Он не удаляет orphan-строки и не меняет `kind` существующего UID. Payload и
manifest обновляются общим fail-closed атомарным протоколом; pending
`export_manifest.json.tmp` не считается стабильным владельцем.

## 5. `.material` и канонизация texture paths

Файл `<sanitized_name>__<uid8>.material` содержит, например:

```json
{
  "schema": "mh.material",
  "schema_version": 1,
  "uid": "7d99d3ac-95ba-44d0-91d5-b95f8a9fce90",
  "name": "m_metal",
  "shader_class": "rendinst_simple",
  "params": {},
  "textures": {
    "tex0": "common/textures/metal_a_tex_d.tif",
    "tex2": "D:/dagor-library/metal_a_tex_n.tif"
  }
}
```

`textures: {texN: string}` — единственная форма. Поле `external_path` не
существует: internal/external вычисляется из представления пути.

- relative path с `/` — internal и трактуется относительно `source_root`;
- normalized absolute path — external;
- абсолютный source path, находящийся под `source_root`, при экспорте всегда
  переводится в относительную форму;
- Blender `//...` сначала разворачивается относительно сохранённого `.blend`,
  затем проходит то же правило «под root → relative, иначе absolute»;
- `//...` в несохранённом `.blend` — блокирующая ошибка;
- пустой texture slot не сериализуется.

Reader классифицирует путь только по relative/absolute форме и не хранит
результат классификации. Поэтому одна текстура имеет одну каноническую форму,
а расположение `source_root` на конкретной машине не попадает в material hash.
External path диагностируется при экспорте и импорте согласно
`texture_policy`. Существование файла не является условием копирования: система
хранит ссылку и никогда не создаёт texture payload.

Материал без dagormat либо с пустым shader экспортируется как
`rendinst_simple` с пустыми `params` и `textures`. Lossless canonical payload
импортированного материала сохраняется в Custom Properties. Если dagormat RNA
не умеет представить payload без потерь, JSON остаётся авторитетным и выдаётся
`MH_W_MATERIAL_PAYLOAD_FALLBACK`.

## 6. Resolver cascade и владение

Resolver работает одинаково для `static_mesh`, `composite` и `material`:

1. Registry даёт приоритетные hints: обязательный `source_path` payload и
   необязательный `manifest_path`.
2. Полный scan всех файлов с точным именем `export_manifest.json` под
   `source_root` подтверждает hint, manifest-row и единственность владельца.
3. Успешный resolve возвращает payload, owning manifest и row. Не найденный UID
   становится unresolved; два owning manifests для одного UID дают
   blocking ambiguity error — первый найденный никогда не выбирается.

Registry не является источником истины и не обходит полный scan. Stale,
invalid, wrong-kind или выходящий за `source_root` hint даёт warning и fallback
к подтверждённому scan-результату. Дискового cache индекса в v1 нет.

Перед apply importer фиксирует набор прочитанных manifests и проверяет, что ни
один из них не стал pending/изменённым в ходе транзакции. Нестабильный source
set блокирует apply, не смешивая ревизии.

Экспортная валидация вычисляет зависимости из payload:

- unresolved mesh/composite reference блокирует экспорт с
  `MH_E_UNRESOLVED_EXTERNAL`;
- unresolved material UID даёт warning со списком и **Export materials…**, но
  не блокирует экспорт геометрии;
- никаких dependency rows в manifest после проверки не записывается.

Production-reader принимает только финальную v1 форму. Pre-freeze
`materials[]` не поддерживается: golden artifacts регенерируются, а локальные
тестовые выгрузки при необходимости обрабатывает одноразовый migration script,
не постоянная ветка кодека.

## 7. Recursive Composite Import

Импорт начинается с выбранного `.composite`, не с FBX:

1. Preflight читает root composite, извлекает resource UID и запускает cascade.
2. Каждый найденный child `.composite` читается и добавляет свои UID edges.
3. Граф дедуплицируется по ResourceUID: один ресурс на любой глубине создаёт
   одну definition Collection.
4. Найденные FBX импортируются в соответствующие mesh Collections; найденные
   `.material` применяются к слотам по MaterialUID.
5. Composite Collections получают Empty Collection Instances с NodeUID,
   target ResourceUID, parenting, transform и Custom Properties.

Точное host-mapping `mh_kind`: definition Collection имеет `static_mesh` или
`composite`; node Empty имеет ровно JSON node kind `group`, `mesh` или
`composite_ref`. Для `group` отсутствуют `mh_resource_uid` и
`instance_collection`; для `mesh`/`composite_ref` они required и target
Collection имеет соответственно `mh_kind=static_mesh`/`mh_kind=composite`.

Все definitions являются sibling Collections под `GEOMETRY`. Граф выражают
Empty-объекты с `instance_collection`, а не физическая вложенность Collections.
Target Collection может быть пустой; ссылка на неё остаётся валидной и позже
наполняется тем же объектом Collection.

Если resource не найден, импорт продолжается. Узел сохраняет NodeUID и target
ResourceUID. Placement Empty хранит `mh_uid = NodeUID`, `mh_resource_uid =
ResourceUID`, `mh_kind=mesh|composite_ref` и `mh_unresolved = true`; target
Collection хранит `mh_uid = ResourceUID`, соответствующий
`mh_kind=static_mesh|composite` и `mh_unresolved = true`. Empty имеет
красный cube-display; лог объясняет, что source не найден под `source_root`.
**Resolve Missing** повторно резолвит такой UID, рекурсивно собирает найденное
поддерево, наполняет те же сущности и переводит `mh_unresolved` в `false` без
замены существующих Empty или Collection.

Back-edge composite cycle во время Blender-import становится unresolved-
placeholder, лог получает `MH_W_COMPOSITE_CYCLE`, остальной граф импортируется.
Blender-export и UE-import блокируются на том же факте кодом
`MH_E_COMPOSITE_CYCLE`.

Нормативная инварианта реестра диагностик: `MH_E_*` всегда блокирует текущую
операцию, `MH_W_*` никогда её не блокирует. Суффикс кода называет обнаруженный
факт, а реакцию конкретного importer/exporter описывает сообщение.

## 8. Acceptance

### M8 — external resource

1. `libB/export_manifest.json` владеет composite `lamp_set` и static mesh
   `lamp_mesh`; `lamp_set.composite` ссылается на `lamp_mesh`.
2. Root composite из `libA` ссылается на ResourceUID `lamp_set`, не сохраняя
   путь.
3. Импорт через общий `source_root` находит owning manifest в `libB`, рекурсивно
   читает `lamp_set.composite`, импортирует `lamp_mesh` FBX и создаёт по одной
   definition Collection на каждый UID.
4. При удалённом `libB` ссылка `lamp_set` становится красным unresolved
   composite-placeholder; после
   возврата каталога **Resolve Missing** наполняет ту же Collection.
5. Два manifests с одним UID дают ambiguity error.

### M9 — shared material

1. Единственный `.material` в `common/materials/` принадлежит своему manifest.
2. Два mesh resources из разных каталогов ссылаются на его MaterialUID.
3. Импорт создаёт/резолвит один Blender Material.
4. Изменение `.material` даёт UPDATE_PROPERTIES материала и ноль mesh updates.
5. Rename материала обновляет содержимое существующего файла без rename пути.

### Owner field slice

В чистом `.blend` импортируется только верхний `.composite`. Автоматически
подтягиваются все найденные nested composites, FBX и materials; отсутствующие
ссылки видимы и восстанавливаются через **Resolve Missing**. Повторный export
даёт те же UID и semantic graph. Texture-policy проверяется одним internal и
одним external authored path без копирования файлов.
