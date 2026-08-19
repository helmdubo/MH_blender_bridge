# C1 slice — composite import path под Clean Sources v2 (handoff)

Статус: **первый C1-срез, не полный gate C1**. Ветка `codex/ue-c0` слита с
`main` (`d277231`, Source Protocol v2); весь новый код написан против
активных контрактов `05` (v2) и `07` (v2). Manifest-resolver v1 не
реализовывался и не будет: он superseded.

## Что входит в срез

- `MHCompositeCodec` (Runtime): строгий reader `mh.composite` **v2**
  (top-level `properties`, unknown-field fail-closed, reserved kinds,
  `MH_E_PARENT_CYCLE`, `MH_E_INVALID_SCALE`, quat normalized+sign-canonical
  через frozen canonical library). v1 spelling даёт маркер
  `MH_W_LEGACY_COMPOSITE_V1_MIGRATION_REQUIRED` и не попадает в candidate set.
- `MHMaterialCodec` (Runtime): строгий reader `mh.material` v1
  (self-identity для resolver).
- `UMHCompositeAsset` (Runtime) по 07 §2: CompositeUid, Name,
  ResourceProperties, flat nodes (ResourceUID — истина, ResolvedAsset — слот
  для импортёра), SourceJsonSnapshot, AssetImportData.
- `MHReadFbxPassport` (Editor): Carrier B `mh_fbx_passport` — consensus по
  всем MESH Model nodes, полная валидация полей v1; любое нарушение —
  `MH_E_PASSPORT_INVALID` (quarantine).
- `IMHSourceResolver` + `FMHPayloadScanResolver` (Editor): рекурсивный скан
  трёх primary payload types под `source_root`, embedded identity, conflict
  matrix 05 §9: duplicate identical → `MH_W_DUPLICATE_IDENTICAL_PAYLOAD`,
  divergent → `MH_E_DIVERGENT_REVISIONS`, отсутствие →
  `MH_E_RESOURCE_NOT_FOUND`; per-file quarantine не валит скан.
- `MHWalkCompositeWave` (Editor): рекурсивная волна `composite_ref` с
  дедупликацией по UID и `MH_E_COMPOSITE_CYCLE`.
- `UMHCompositeAssetFactory` (Editor): `.composite` v2 → `UMHCompositeAsset`
  в Content Browser + reimport-in-place (FReimportHandler).
- `-run=MHCompositeDump <file> [-root=...]`: слепок иерархии композита;
  с `-root` — скан, рекурсивное раскрытие и resolve-статусы. Exit code 1 при
  любом `MH_E_*`.
- Automation: `Mimir.C1.CompositeCodec`, `Mimir.C1.PayloadResolver`,
  `Mimir.C1.CompositeWave` (golden root резолвится как в C0: аргумент → env →
  чекаут).

## Что осознанно отложено (до полного C1/C2)

- Ledger + analyzer/классификация изменений (`NO_CHANGE`/`UPDATE_*`/`MOVE`)
  и parity M8/M9/M10 — следующий шаг C1.
- Byte-parity канонической one-line формы паспорта с Python-эталоном
  (сейчас: структурная валидация + консенсус; TODO(C1-parity) в коде).
- Проверка канонической сортировки `nodes` по `node_uid` reader'ом —
  сейчас reader лениво допускает несортированный массив; вопрос к ревьюверу.
- Startup scan/watcher, `-run=MHImportSources`, geometry import через
  `FMHFbxBackend` в фабрики (C2).

## Расхождение, требующее решения ревьювера

`05` §4.2 словами называет carrier property `mh.fbx_passport`, а production
writer (`addon/mh4blend/core/fbx_passport.py`, `PASSPORT_PROPERTY`) пишет
`mh_fbx_passport`. UE reader читает `mh_fbx_passport` (имя из writer).
См. `docs/QUESTIONS.md` UE-QUESTION-13.

## Статус валидации

Код написан в облачной сессии без UE 5.7 toolchain. Первая сборка на машине
владельца (VS 2022, `MimirHead_portfolioEditor Win64 Development`) выявила две
ошибки, обе исправлены:

- unity-build склеивал `MHCompositeCodec.cpp` и `MHMaterialCodec.cpp`, у
  которых совпадали имена helper'ов в анонимных namespace (`Invalid`,
  `SerializeCompactObject`) — C2084. Общий сериализатор вынесен в
  `Private/Codec/MHCodecJson.h`, диагностические helper'ы разведены по именам
  (`InvalidComposite` / `InvalidMaterial`);
- `MHCompositeDumpUtil.cpp` затенял локальную `Error` переменной цикла —
  C4456, а модуль собирается с `bWarningsAsErrors`.

Модули `MimirCompositeTests` и остальной Editor-код компилятор принял
полностью. Следующий шаг: пересборка, `Automation RunTests Mimir`
(`Mimir.C0.*` + `Mimir.C1.*`), затем полевой прогон — Import `.composite` в
Content Browser и Tools → Mimir → дампы.

## Второй C1-срез: settings + hashes + Ledger + analyzer

Добавлено поверх первого среза (всё ещё **не полный gate C1**):

- `UMHCompositeSettings` (`UDeveloperSettings`, `config=Editor`,
  `defaultconfig`) по `07` §11: `SourceRoot`, `ContentRoot` (`/Game/MH`),
  `MasterRoot`, `TextureRoot` (пусто = `SourceRoot`), `TexturePolicy`
  (`Transitional` по умолчанию), `ConflictPolicy` (`Prompt`),
  `StartupScanMode` (`Silent`), префиксы `SM_/MI_/T_/CA_`,
  `LumenCardsMax=32`, `GeometryBackend=MhFbx`.
- `MHPayloadHashes` (Editor): три величины `05` §4.3 —
  `MHCompositeSemanticHash` и `MHMaterialSemanticHash` (canonical form
  frozen-библиотеки + xxh3; material хеширует только
  `{shader_class, params, textures}`, поэтому rename материала — `MOVE`,
  а не `UPDATE_PROPERTIES`), `MHPassportDescriptorHash` (документ паспорта,
  пересобранный из провалидированных полей без `geometry_hash`) и
  `MHPayloadFingerprint` (xxh3 по сырым байтам).
- `FMHLedgerRow` + `UMHImportLedger` (`<content_root>/_MH/Ledger`) по `07` §2
  и независимый JSON-снимок (`mh.import_ledger:1`) для headless/commandlet/
  тестов. Снимок — reader state: он живёт под `Saved/`, в `source_root` его
  писать нельзя.
- `MHAnalyzeSources` (Editor): пер-ресурсная классификация `07` §4 / `05` §9 —
  `CREATE`, `UPDATE_GEOMETRY`, `UPDATE_DESCRIPTOR`, `UPDATE_PROPERTIES`,
  `MOVE`, `NO_CHANGE`, `NO_CHANGE_EXTERNAL`, `REMOVE`, `BLOCKED`. Волны и
  зависимости в анализатор не входят (batch semantics `07` §1).
  Изменившийся fingerprint при равных semantic hashes даёт
  `MH_W_PAYLOAD_EXTERNAL_MODIFIED` **и**
  `MH_E_EXTERNAL_MODIFICATION_CONFIRMATION_REQUIRED`; такая строка Ledger
  молча не продвигается. `MH_E_*` блокирует только свой ресурс.
- `-run=MHAnalyzeSources -root=<source_root> [-ledger=…] [-writeledger=…]
  [-report=…]`: скан + анализ, строка на ресурс, опциональный JSON-отчёт
  (`mh.analyze_sources:1`) и продвинутый снимок Ledger. Без `-root` берётся
  `SourceRoot` из project settings; нет ни того, ни другого — usage и exit 2.
  Exit 1 при любом `MH_E_*`.
- Automation: `Mimir.C1.Analyzer` (CREATE → NO_CHANGE → MOVE →
  UPDATE_PROPERTIES → REMOVE → NO_CHANGE_EXTERNAL → divergent BLOCKED) и
  `Mimir.C1.LedgerSnapshot` (round-trip строк снимка).

### Решения, где контракт оставлял свободу

- `IMHSourceResolver::Resolve` требует ожидаемый kind, а анализатору kind
  заранее неизвестен: он пробует три kind'а и берёт первый ответ, отличный
  от `KindMismatch`. Поведение и диагностики resolver'а не изменились;
  в `FMHResolveOutcome` добавлен только `Fingerprint`, в скан-resolver —
  `GetAllUids()`.
- Порядок сравнения: geometry → descriptor → path → fingerprint. Значит
  переезд файла с одновременной правкой геометрии классифицируется как
  `UPDATE_GEOMETRY` (перемещение отработает тот же re-import).
- Строка Ledger с пустым `payload_fingerprint` не поднимает
  confirmation gate: отсутствие записанного fingerprint ничего не доказывает.
- `ImportStatus` в C1 хранит саму классификацию; на C2 её заменит результат
  реального импорта.
- Карантин (`MH_E_PASSPORT_INVALID` и подобные пер-файловые отказы) остаётся
  warning'ом анализа: он и так исключён из candidate set и не должен ронять
  exit code всего скана.

### Что всё ещё блокирует полный C1

- Parity M8/M9/M10 против Blender-эталонов: golden-фикстур со стороны
  Blender ещё нет.
- Byte-parity `descriptor_hash` с Python (`sha256` от `json.dumps`) не
  заявляется: UE-хеш самосогласован внутри Ledger, TODO(C1-parity) в
  `MHPayloadHashes.h`.
- FBX/passport строки анализатора не покрыты автотестом: в репозитории нет
  ни одной FBX-фикстуры с паспортом Carrier B, а подделывать её нельзя.
  Появится фикстура — сценарии `UPDATE_GEOMETRY`/`UPDATE_DESCRIPTOR`
  добавляются в `Mimir.C1.Analyzer` без изменения кода анализатора.
- Startup scan/watcher, `-run=MHImportSources` и фактический импорт (C2/C3):
  `UMHImportLedger::Save()` уже пишет пакет, но продвигает Ledger пока только
  commandlet-снимок.

## Полевые наблюдения по реальному payload (2026-08-19)

Владелец предоставил дамп реального `sovmod_garage_shell_a_type_a.mesh.fbx`
из production writer `mh4blend 0.6.0`:

- Carrier B паспорт валиден по всем правилам UE-ридера: 11 полей, dense
  `lod_levels`, слоты отсортированы по `slot_name`, byte-consensus на двух
  MESH-узлах. Дополнительные свойства (`mh_uid`,
  `mh_imported_resource_uid`) корректно игнорируются.
- **Интел для C2 (уточнён по полному дампу).** В node-транзформах реальных
  payload'ов лежат две независимые величины:
  - `local_rotation` −90.000009° по Z — это axis-конверсия транспорта. Файл
    объявляет «вперёд = −X», UE требует «вперёд = −Y», поэтому
    `FbxAxisSystem::ConvertScene` добавляет ровно +90° по Z и гасит её.
    Подтверждено R1: `axis_probe` несёт ту же подпись (−90.000003°), и оба
    бэкенда сошлись на `(37, −11, 193)`, хотя прямой трансформ узла не читает;
  - `local_scaling` 0.01 — **не** конверсия единиц, а собственный масштаб
    объекта, и он семантически обязателен. Bbox по 911 control points даёт
    `2.81 × 5.31 × 2.15` м с этим масштабом и абсурдные `281 × 531 × 215` м
    без него. `ConvertScene` масштаб не трогает, поэтому штатный импортёр UE
    (`bTransformVertexToAbsolute`) даёт верный размер, а текущий
    `FMHFbxBackend`, читающий control points сырыми, выдал бы меш в 100 раз
    больше.
  Отсюда правило маппера C2: `UE_local_cm = ConvertPos(Rz(+90°) ·
  NodeGlobalTransform · raw_cp)`; практически — вызвать `ConvertScene` и
  запечь `EvaluateGlobalTransform`, как делает утверждённый parity-эталон.
- Трансформ объекта **входит** в `geometry_hash`: `meshser.py`
  сериализует `MeshObjectRecord.transform` (16 float, row-major, collection
  space, `P_TRANSFORM = 6`) первым в потоке `mh.meshser:2`. Дыры в
  reader-side diff нет — изменение масштаба меняет хеш.
- **Слоты ремапятся только по `slot_name`.** В этом payload порядок слотов
  FBX (`0=sovmod_garage_shell_a`, `1=Material001`) обратен паспортному
  (`0=Material001`, `1=sovmod_garage_shell_a`), а у второго узла таблица
  вообще из одного слота. Наивное использование `material_index` как индекса
  слота UE поменяло бы материалы местами на всех 762 полигонах.
- Два render mesh-узла на одном LOD0 и имена с суффиксом `_lod00`
  подтверждают контракт: имена не семантика, уровень задаёт только
  `mh_lod_level`.
- Passport-carrying FBX fixture в `golden/fixtures/` по-прежнему нет;
  как только Blender-сторона закоммитит одну, FBX-сценарии добавляются в
  `Mimir.C1.Analyzer` и фиксируются expected-дампы.
