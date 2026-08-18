# ADR-V2 — MH Source Protocol v2: Passport-First (направление принято, реализация через spikes)

Статус: этот документ ЗАМЕНЯЕТ отозванный AMENDMENT v1.1. Направление
passport-first принято владельцем как ADR; текущая **Source Schema v1 остаётся
действующим контрактом** до приёмки v2 (dual-read на весь переход). Ярлык
«v1.1» упразднён: меняется authority-модель — это **MH Source Protocol v2**.
Версии форматов независимы: `mh.fbx_passport:1` (новый),
`mh.material` schema_version 1 (без изменений), `mh.composite` schema_version 2
(перенос resource-properties внутрь), `mh.local_index` — implementation detail.

Реализация НЕ начинается с переписывания экспортёра. Порядок: spikes G1–G4
(§8) → фиксация v2-нормы → миграция. Исключения, которые можно делать сразу,
перечислены в §9.

**Combined-LOD override — 2026-08-18:** обязательный документ
`AMENDMENT_combined_lod_fbx.md` заменяет per-file LOD-части §1.1, §1.2, §1.6
и G1/G3 ниже. Один mesh UID имеет один FBX; паспорт заявляет `lod_levels`, а
фактический уровень хранится как `mh_lod_level` на mesh Model node. Geometry
hash использует `mh.meshser:2`.

## 1. Принятые решения (по итогам внешнего ревью; в Decision Log отдельными ADR)

### 1.1 Три величины вместо одного content_hash

| Величина | Что это | Кто считает | Где живёт |
|---|---|---|---|
| `geometry_hash` | семантический hash mh.meshser:2 всех LOD (evaluated Blender-геометрия, до cm) | только Blender-экспортёр | паспорт FBX + local index |
| `descriptor_hash` | hash канон-формы паспорта БЕЗ самого поля hash | обе стороны | local index (в паспорт не пишется) |
| `payload_fingerprint` | byte-hash (или size+mtime fast-path) записанного файла | кто угодно | только local index |

Rebuild/scan никогда не «пересчитывает» geometry_hash из FBX — он читает его
из паспорта и сверяет payload_fingerprint. Расхождение fingerprint при
неизменном паспорте = файл менялся сторонней программой → статус
`external_modified` (`MH_W_PAYLOAD_EXTERNAL_MODIFIED`), ресурс требует
подтверждения, не считается корректным молча.

### 1.2 Паспорт FBX — полный состав (канонический JSON, одна строка-property)

```json
{
  "schema": "mh.fbx_passport",
  "schema_version": 1,
  "resource_uid": "2db5574c-…",
  "kind": "static_mesh",
  "name": "wall_a",
  "lod_levels": [0, 1],
  "lod_policy": "authored",
  "geometry_hash": "xxh3:9f2c01ab34cd56ef",
  "material_slots": [
    { "slot_name": "wall_surface",
      "material_uid": "7d995e54-…",
      "material_name_hint": "m_stucco" }
  ],
  "properties": { "role": "wall" },
  "exporter": "mh4blend 0.x"
}
```

- `material_name_hint` — только диагностика («material 7d99 (m_stucco) не
  найден»), в identity и в descriptor_hash участвует, но никогда не заменяет
  `.material.name` и не используется для резолва.
- Один UID имеет ровно один combined FBX и один общий geometry_hash. Паспорт
  заявляет полный `lod_levels`; фактический integer `mh_lod_level` хранится на
  каждой mesh Model node. Scanner не выводит уровень из имени файла/узла.
- `.composite`: resource-level properties переносятся ВНУТРЬ файла
  (top-level `properties`) — это и есть bump до `mh.composite` schema_version 2.
- `.material` самодостаточен уже сейчас; его текстурные зависимости — внешние
  по определению (см. §5).

### 1.3 Правило записи FBX (закрывает stale passport)

```
write_fbx = geometry_hash_changed OR descriptor_hash_changed
            OR payload_missing OR recovery_required
```

Metadata-only изменения (MaterialUID слота, rename, properties, lod_policy)
обязаны переписывать файл. Тест-мутации на каждый случай — в G2-spike.

### 1.4 Carrier паспорта — выбирается spike'ом G1, кандидат по умолчанию — B

Вариант B: паспорт-JSON дублируется custom property на КАЖДУЮ экспортируемую
Model-ноду; ридер собирает все копии и требует побайтового равенства
(расхождение = malformed → quarantine). Альтернативы A (sentinel Empty) и
C (пост-запись через parser) остаются в G1 как запасные. Проверить оба
Blender FBX-пути (python и новый нативный C++ на базе ufbx), влияние на
pivot/иерархию, длинные JSON, shared datablocks, custom normals.

### 1.5 Conflict matrix (заменяет «паспорт всегда выигрывает»)

| Состояние | Реакция |
|---|---|
| UID неизвестен проекту | Adopt as new resource |
| UID известен, старый файл исчез, новый один | MOVE (авто, лог) |
| UID известен, оба файла есть, fingerprints равны | duplicate-copy warning (предложить удалить копию) |
| UID известен, оба файла есть, fingerprints различны | **hard conflict** `MH_E_DIVERGENT_REVISIONS` — только ручной выбор |
| Loose file выбран вручную | диалог: Update existing / **Fork as New Resource** / Cancel |
| Паспорта нет | legacy-резолв через манифест (dual-read) либо явный adopt |
| Паспорт malformed / незнакомая версия | quarantine, `MH_E_PASSPORT_INVALID` |

**Fork as New Resource** — обязательная операция: новый ResourceUID; при
форке композита — перезапись внутренних ссылок на форкнутые же ресурсы по
выбору пользователя; оригинальные UID проекта не затрагиваются.

### 1.6 Index — локальный производный кеш ВНЕ дерева исходников

- Расположение: `%LOCALAPPDATA%/MimirHead/MHBridge/<project_uid>/index.json`
  (и аналог на других ОС). В source tree служебных файлов НЕТ вообще —
  требование владельца о стерильном дереве выполняется буквально.
- Формат — JSON (два независимых кодека Python/C++, диагностика глазами);
  sqlite отклонён. Побайтовое совпадение сериализаций двух реализаций НЕ
  требуется — это кеш, нормируется только payload-контракт.
- Содержимое — честная картина кандидатов, не только UID→path:
  `path → {fingerprint, parsed_passport|parse_status}`;
  `uid → {candidate_paths[], resolved|conflict status, lod_levels}`.
- Index удаляем в любой момент без потери чего-либо; Blender и UE держат
  каждый свой.

### 1.7 Согласованность: eventual, транзакция «payload+index» упразднена

Payload публикуется атомарно (tmp→rename, как раньше) и является истиной.
Index догоняет: watcher/скан видит новый fingerprint → перечитывает паспорт →
чинит кеш. Lock нужен только против одновременной записи ОДНОГО payload'а
двумя процессами одной машины (короткий per-file lock при экспорте);
глобальный index.lock и журнальные маркеры по дереву — упразднены. Crash
recovery = обычная работа watcher'а; специального протокола нет, потому что
нет распределённого состояния.

### 1.8 Отклонено (решения владельца)

- **Transfer package (.mhpack) — отклонён.** Внутренняя пересылка — loose
  files: получатель кладёт файл(ы) в дерево, система читает паспорта и
  называет недостающие зависимости по имени (name_hint). Дозапросить файл у
  отправителя — нормальный человеческий шаг; сборка closure перед отправкой —
  перекладывание машинной работы на человека. Для ВНЕШНЕЙ передачи
  (клиент/аутсорс) остаётся Export Selection (dependency closure) в ROADMAP.
- Центральный `.mh/` в source tree — отклонён (см. 1.6).
- Бинарный index — отклонён (ROADMAP-порог прежний).

## 5. Текстуры: резолв по basename и актуализация путей (dag4blend-паритет)

Требование владельца: как в dag4blend — если текстура существует где-то в
дереве проекта, система находит её по конечному имени файла и актуализирует
путь в материале.

- Идентичность текстуры остаётся путём (D23 в силе), но резолв получает
  каскад: **точный путь → поиск по basename под texture_root → unresolved**.
- Поиск по basename: ровно одно совпадение → путь считается устаревшим,
  предлагается/выполняется актуализация; несколько совпадений →
  `MH_W_TEXTURE_BASENAME_AMBIGUOUS`, автопочинки нет, выбор за пользователем;
  ноль → unresolved с именем файла в диагностике.
- **Актуализация — это правка `.material`** (путь в `textures.texN`):
  выполняется (а) автоматически при Blender-импорте материала со stale-путём
  и единственным кандидатом (поведение dag4blend), с записью в лог;
  (б) массовой операцией **Actualize Texture Paths** (аддон-оператор +
  UE-commandlet): пройти по всем материалам, починить пути, отчёт
  fixed/ambiguous/missing. Правка легально меняет hash материала →
  штатный UPDATE_PROPERTIES → MI обновится обычным конвейером.
- Инвариант для дерева: дубликаты basename в texture_root легальны, но
  исключают автопочинку затронутых имён — Actualize-отчёт их перечисляет;
  рекомендация воркфлоу — уникальные имена текстур (в Dagor это фактическая
  норма).
- Local index кеширует таблицу `basename → paths[]` для скорости; истина —
  дерево.

## 8. Spike-gates (по ревью; выполняются ДО правки нормативных доков)

- **G1 Passport transport** (первый, блокирующий): carrier B против A/C на
  реальных файлах — один/несколько объектов, shared datablock, custom
  normals, длинный JSON, LOD-набор; оба Blender FBX-пути; чтение в UE
  (SDK) и в Blender (io_scene_fbx / собственный минимальный ридер);
  re-export; отсутствие влияния на pivot/иерархию. Выход: зафиксированный
  carrier + замеры.
- **G2 Metadata-only update**: пять мутаций из 1.3 — паспорт и index нигде
  не остаются stale.
- **G3 Rebuild & conflicts**: вся матрица 1.5 + combined-LOD passport/node
  mismatch + missing
  material + legacy без паспорта + malformed + Fork.
- **G4 Crash semantics**: crash до/после rename payload'а; два писателя;
  Blender+UE одновременно; удалённый/устаревший index; копирование через
  sync-папку.

Только после зелёных G1–G4: правка 05 → v2, миграционная утилита
(манифесты → паспорта, dual-read до полного покрытия, отчёт), удаление
распределённых манифестов из воркфлоу.

## 9. Что делать сейчас (не дожидаясь spikes)

- **Blender-исполнитель**: G1-spike — первая задача (ветка, вне main).
  Параллельно легально: перенос composite resource-properties внутрь файла
  (mh.composite v2 — независимый bump, нужен при любом исходе);
  Actualize Texture Paths (§5 — не зависит от паспортов);
  правило записи 1.3 как рефакторинг экспортёра (полезно и в v1).
- **UE-исполнитель**: этап C продолжается ПРОТИВ ДЕЙСТВУЮЩЕЙ v1 без
  остановки; резолвер держать за интерфейсом (seam уже есть) — замена
  manifest-scan на passport/index-резолв при приёмке v2 не должна трогать
  ничего выше интерфейса. Чтение паспорта FBX добавить в FMHFbxBackend как
  опциональное (если есть — сверка с манифестом, warning при расхождении):
  это безопасно в v1 и станет обязательным в v2.
- **Ревьюверу**: этот ADR — на второй круг вместе с результатами G1.

## 10. Правки документов (после G-gates, не сейчас)

05 → MH Source Protocol v2 (authority: passport-first; resolver: index-as-
cache; манифесты — legacy-read); 04 — workflows loose/fork/actualize;
07 — §3 резолвер за seam, §10 watcher по fingerprints; Decision Log — ADR'ы
из §1 + отклонения из 1.8; golden — переписать M8/M9/M10 + матрица 1.5 как
негативный набор.
