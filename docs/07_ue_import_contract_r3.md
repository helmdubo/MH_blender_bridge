# 07 — UE Import Contract v1 (r3, к передаче в разработку)

Статус: контракт этапа C для плагина `MimirComposite` (UE **5.7.4**, совместимость
5.8). Работает ПРОТИВ действующей Source Schema v1 (docs/05). ADR-V2
(passport-first) принят как направление и учтён здесь только двумя вещами:
резолвер строится за интерфейсом (замена реализации при приёмке v2 не трогает
ничего выше), чтение FBX-паспорта — опциональная сверка. Всё остальное v2 —
вне скоупа C. Компилятор/актёр/Rebuild — этап D, отдельный контракт.

r3 = r2 + консолидация решений после ревью: слот-сверка, Verify Materials,
Export Material to Source (force), resolver seam, паспорт-чтение, именованные
C2-кейсы.

Модули: `MimirCompositeRuntime` (классы ассетов), `MimirCompositeEditor`
(импорт, синхронизация, UI, commandlets), `MimirCompositeTests`.

## 1. Транзакции

Blender экспортирует standalone per-resource; UE импортирует батчем по
стабильному снапшоту manifest-set под `source_root` (05 §9.2; pending
`*.tmp`-маркер или изменение манифестов между preflight и apply →
`MH_E_INVALID_EXPORT_MANIFEST`, отмена). Импорт не транзакционен на уровне
пакетов: пер-ресурсная изоляция ошибок (`MH_E_*` блокирует свой ресурс, батч
продолжается), Ledger коммитится только для успешных, единый отчёт.
Инвариант диагностик: `MH_E_*` блокирует операцию, `MH_W_*` — никогда.

## 2. Классы

**`UMHCompositeAsset`** (Runtime): образ `mh.composite`: CompositeUid, Name,
TArray<FMHCompositeNode>{NodeUid, ParentUid, Kind, DisplayName, ResourceUid,
LocalTransform, PropertyBag}, SourceJsonSnapshot, AssetImportData. Read-only
(VisibleAnywhere). Reserved kinds → `MH_E_UNSUPPORTED_NODE_KIND`. Узел несёт
ResourceUid (истина) + FSoftObjectPath ResolvedAsset (заполняет импортер —
Reference Viewer/кук бесплатно).

**`UMHImportLedger`** (Editor-домен, ассет в `<content_root>/_MH/Ledger`):
`FGuid → {Kind, Asset, SourceRelPath, AppliedContentHash, ImportedAt}` (+
mtime/size текстур). Производный артефакт: потеря = полный реимпорт.

**`UMHManifestImporter`** (EditorSubsystem): **единственный публичный вход**
`ImportSources(Scope)`; стадии приватные, не экспонируются; частичность —
только диффом:

```
Snapshot → Resolve → Analyze/Diff → Plan
→ Textures → Materials → Geometry → Composites → Finalize
→ LedgerCommit → Report
```

## 3. Resolver — ЗА ИНТЕРФЕЙСОМ (требование ADR-V2)

`IMHSourceResolver`: `Resolve(uid) → {payload_path, owning_manifest_path,
manifest_row} | Unresolved | Ambiguous`. Реализация этапа C —
`FMHManifestScanResolver`: порт 05 §6 (registry-hint с подтверждением полным
сканом; единственный owning manifest; ноль владельцев = unresolved, два =
`MH_E_AMBIGUOUS_RESOURCE_OWNER`; сканируются только `export_manifest.json`;
дискового кеша нет). Будущая v2-реализация (passport/local-index) обязана
встать под тот же интерфейс — выше интерфейса зависимостей от способа
резолва быть не должно (проверяется ревью C1).

Канон-библиотека C++ (квантование, NFC через ICU движка, канон-JSON, XXH3-64,
path-канонизация 05 §5.3) обязана проходить `golden/canonical_vectors.json` —
gate C0, блокирующий.

## 4. Analyzer / дифф / отчёт

Структурное сравнение (snapshot, Ledger); content_hash — fast-path. Формат
`mh.diff_report` v1 (CREATE/REMOVE/RENAME/MOVE/UPDATE_GEOMETRY/
UPDATE_TRANSFORM/UPDATE_PROPERTIES/REPARENT/UPDATE_RESOURCE/UPDATE_KIND/
EXTERNAL_UNRESOLVED). **Parity-gate:** на golden-наборах отчёт байт-в-байт
равен `tools/diff_bundles.py`. UE-only флаги `LOCAL_EDIT`/`CONFLICT` — вне
parity. Отчёт: Message Log (категория Mimir) + JSON в Saved/ для CI.

## 5. Textures

Идентичность = путь; один файл → один UTexture2D;
`<content_root>/<путь под source_root>/T_<basename>` (D27-зеркало). External
absolute (transitional) → `MH_W_TEXTURE_OUTSIDE_ROOT`, в
`<content_root>/_External/<hash>/`. Суффиксы: `*_d|*_tex_d` → sRGB on;
`*_n|*_tex_n` → sRGB off + Normalmap; `*_m|*_tex_m` → sRGB on. Реимпорт по
mtime/size (Ledger). Basename-каскад и Actualize (ADR-V2 §5) — Blender-сторона;
UE-commandlet `MHActualizeTexturePaths` — задача пост-C3, в C-гейты не входит.

## 6. Materials

Master: `shader_class → <master_root>/<shader_class>` (+ alias-map); не
найден → `MH_E_MASTER_MATERIAL_NOT_FOUND` (блокирует только материал).
MI `<content_root>/<путь>/MI_<name>`, parent=Master; скаляр→Scalar,
массив4→Vector(LinearColor), texN→TextureParameter `texN`; ключ вне Master —
warning.

**Трёхстороннее сравнение (D25):** base=AppliedContentHash (Ledger),
theirs=hash `.material`, ours=канон-хеш фактических параметров MI.
theirs≠base ∧ ours=base → молчаливый UPDATE; theirs=base ∧ ours≠base →
`LOCAL_EDIT` (MI не трогать); оба ≠ → `CONFLICT` → `conflict_policy`
(prompt|overwrite|keep; UI prompt, headless overwrite).

### 6.1 Export Material to Source (writeback, решение владельца — force)

Явная операция на MI (кнопка в Details/контекст CB + batch из Verify
Materials): дамп фактических параметров → канон-форма → **перезапись
`.material`** атомарным протоколом + manifest upsert + Ledger
(AppliedContentHash=новый; LOCAL_EDIT снимается по построению). Перед записью —
диалог с построчным диффом «файл ↔ MI»; если файл новее применённого
(theirs≠base) — явная строка «файл содержит неприменённые изменения, будут
потеряны». Force: подтверждение перезаписывает всегда. Отказ только при
непереносимом (параметр вне схемы, static switch, переопределённый parent) —
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE` с перечнем. Каждый writeback — событие в
Message Log/отчёте.

## 7. Geometry — `FMHFbxBackend` (основной backend)

Прямой FBX SDK (third-party модуль движка), без UFbxFactory.

- **Сырая сцена**: ConvertScene не вызывается; конверсия — своя по §11 схемы
  (подтверждена R1). Заявленные файлом axis/units сверяются с канон-настройками
  экспорта; расхождение → `MH_E_GEOMETRY_SOURCE_MISMATCH`.
- Узлы: render-меши → FMeshDescription (перечень полей зеркален §9 mesh-hash:
  позиции, полигоны, per-face material index, split normals, smoothing,
  UV-слои, color attrs); `UCX_*` → collision; `SOCKET_*` → sockets; прочее —
  warning, игнор.
- **Слот-сверка**: PolygonGroup-имена файла сверяются со `slot_name`
  manifest-row; расхождение в любую сторону → `MH_W_SLOT_MISMATCH` с перечнем.
  Назначение MI — по material_slots (uid→MI из Ledger) ДО PostEditChange;
  слот без записи → warning + дефолт-материал.
- **Паспорт (опционально в v1)**: custom properties `mh_*`, если присутствуют
  в файле, читаются и сверяются с manifest-row (uid/kind/name); расхождение →
  `MH_W_PASSPORT_MANIFEST_MISMATCH`. Отсутствие паспорта в v1 — норма, без
  warning'а. (Обязательность и authority — v2, вне скоупа.)
- **LOD (Combined-LOD amendment)**: один FBX содержит все уровни. Маппер
  группирует render mesh-узлы по integer custom property `mh_lod_level`
  (`absent` = 0 только для single-LOD) и за один проход собирает
  `SourceModel[N].MeshDescription`. Имена узлов и `.lodNN` suffix не читаются.
  Заявленный в passport `lod_levels` обязан совпасть с фактическим множеством,
  иначе `MH_E_LOD_PASSPORT_MISMATCH`. Sparse levels и slot уровня 1+, которого
  нет в LOD0, делают malformed весь mesh-ресурс. `lod_policy=nanite` → Nanite
  settings on; authored screen sizes в ROADMAP, пока
  `bAutoComputeLODScreenSize=true`.
- **Reimport-in-place (нормативно)**: обновление существующего SM через
  CreateMeshDescription/CommitMeshDescription/Build/PostEditChange в тот же
  объект; пересоздание ассета запрещено.
- Legacy-фолбэк `FLegacyFbxBackend` (UFbxFactory, materials off) — parity-
  эталон и аварийный переключатель (`geometry_backend`, default `mh_fbx`).
- **R1-automation — первый коммит**: axis_probe через оба backend'а против
  чисел RISK_RESULTS.

### 7.1 `mh.fbxdump` (диагностика, вне frozen-контракта)

Commandlet `-run=MHFbxDump <file> [--full]`: сырой граф сцены → канонический
JSON (узлы, TRS как записаны, counts, имена слотов, наличие passport-properties,
`mh_lod_level` каждого mesh-узла и сводка уровней, заявленные
axis/units/exporter). Числа квантованы; полные массивы — `--full`.
Дампы golden-фикстур коммитятся как expected-спецификация маппера. Тег
`mh.fbxdump:1`.

## 8. Composites

Фабрика `.composite` → `UMHCompositeAsset` `<content_root>/<путь>/CA_<name>`
(UFactory+FReimportHandler). Топосорт по вычисленным зависимостям; цикл →
`MH_E_COMPOSITE_CYCLE` (блокируются композиты цикла). Недостающий ресурс →
ассет создаётся, узел unresolved, ошибка в отчёте.

## 9. Finalize

Реестр правил (порт reference-скрипта; имя+условие+действие, лог):
`role=decal` / legacy-суффикс `_decal(s)` (fallback+warning) → маркер-тег;
Lumen Mesh Cards (настройка, 32); UCX-чистка; прочее по README карты
портирования. Только на ассетах текущего плана.

## 10. Синхронизация

- Watcher: IDirectoryWatcher на `export_manifest.json` под root (за тонкой
  абстракцией `IMHChangeDetector` — v2 сменит точку наблюдения на
  fingerprints/local index, выше абстракции изменений быть не должно);
  debounce ≥1s; очередь при PIE.
- Startup-скан: OnAssetRegistryFilesLoaded, сравнение по content_hash (не
  mtime); режим `prompt|silent` (default prompt: «N ресурсов обновились —
  Import All / Show Diff / Later»).
- CI-commandlet `-run=MHImportSources` (headless-политики, exit-code по
  `MH_E_*`).
- **Verify Materials**: кнопка + commandlet `-run=MHVerifyMaterials` — Analyze
  без Execute по ВСЕМ материалам Ledger'а (ours vs theirs), отчёт
  рассинхронов; из отчёта — batch Fix (переприменить из файлов) и batch
  Export to Source (§6.1).
- Registry-генератор: `mh.registry` v1 из Ledger + `shader_classes` сканом
  master_root; Refresh-кнопка + после каждого успешного импорта.

## 11. Настройки (UDeveloperSettings)

`source_root` (обязательная), `content_root` (`/Game/MH`), `master_root`,
`registry_output_path`, префиксы `SM_/MI_/T_/CA_`, `texture_policy`
(transitional), `conflict_policy` (prompt), `startup_scan_mode` (prompt),
`lumen_cards_max` (32), shader_class alias-map, `geometry_backend`
(`mh_fbx`|`legacy`).

## 12. Вне скоупа C

Компилятор/актёр/ActorFactory/Rebuild (этап D); reserved kinds; Break/Build;
ISM/LI; Adopt Existing; Interchange/USD; Skeletal; запись source-файлов из UE
(read-only; исключения: registry-файл и явный writeback §6.1);
v2-authority (паспорт обязателен, local index) — только за seam'ами §3/§10.
Участие в spike G1 (ADR-V2): маленькая задача — подтвердить чтение
паспорт-properties выбранного carrier'а через SDK; по запросу
Blender-исполнителя.

## 13. Gates

- **C0**: каркас 5.7.4; канон-библиотека проходит canonical_vectors;
  fbxdump + закоммиченные дампы golden-фикстур; R1-automation (оба backend'а).
  *Внешний аудит.*
- **C1**: codecs + resolver (за IMHSourceResolver) + analyzer,
  commandlet-testable; diff-parity со всеми golden-мутациями + M8/M9;
  ревью подтверждает отсутствие зависимостей от способа резолва выше seam.
  *Внешний аудит.*
- **C2**: фабрики/Ledger/builders; FMHFbxBackend; mesh-parity против legacy
  (допуски явные); Combined-LOD кейс (правка геометрии lod01 в Blender →
  реэкспорт единого FBX → один `UPDATE_GEOMETRY` ресурса → в UE пересобраны все
  SourceModel, UStaticMesh обновлён in place);
  повторный импорт → пустой дифф, ноль пересозданий; именованные кейсы:
  «material-only edit» (один UPDATE_PROPERTIES, ноль geometry-операций,
  MI тот же объект), «MI drift при неизменном файле» (LOCAL_EDIT, файл не
  затирает), three-way обе политики; слот-сверка. *Внешний аудит.*
- **C3**: watcher + startup-скан + registry + commandlets (ImportSources,
  VerifyMaterials) + writeback §6.1. *Внешний аудит.*
- **C4**: owner field acceptance — сквозной прогон на реальном source_root:
  Export → авто-подхват → LOCAL_EDIT/CONFLICT вручную → writeback →
  Verify Materials → M8/M9 руками. После — контракт этапа D.
