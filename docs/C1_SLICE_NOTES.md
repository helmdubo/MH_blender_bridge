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
