# R5b-0 (Recipe Model v2.1) — reconcile реимпорта на уровне пула

Статус: **REVIEW** (близнец). Подготовительный срез перед R5b-1 (перевод
компилятора размещений, Outliner'а и Undo на пул): пул умеет сам применять
`FMHEndpointInterfaceDelta` (R3a/R3b) к своим бакетам, чтобы R5b-1 не трогал
`MHInstancePool.{h,cpp}` и уложился в шесть файлов.

## 1. Что сделано

- `UMHInstancePoolSubsystem::ReconcileMesh(Mesh, Delta)` — один проход по
  бакетам меша (не по owner'ам), docs/16 §4: payload/bounds → render + bounds
  refresh на месте, тот же компонент; `bBucketDescriptor` → `MigrateBucket`:
  дескриптор перечитывается из меша (`FromMesh`, override-материалы бакета
  сохраняются), новый ISM на том же pool-акторе, живые инстансы переносятся в
  текущем порядке ISM, скрытые слоты остаются скрытыми, хэндлы/owner/NodePath/
  transform не меняются, старый компонент уничтожается; `bCollisionInterface`
  → `RecreatePhysicsState`; `bMaterialBinding` → `EmptyOverrideMaterials`.
  Пустая дельта и чужой меш — ноль касаний. Счётчик R3b
  `MHRecordReimportBucket(bMigrated)` пишется пулом; метрика
  `FMHInstancePoolMetrics::BucketsMigrated`.
- `GetBucketComponents(Mesh, Out)` — живые ISM-компоненты меша (тестовый и
  Outliner-seam R5b-1).
- Ничего в акторе/компиляторе не изменено: R3b-путь актора продолжает
  reconcile'ить свои собственные бакеты до R5b-1.

## 2. Тесты (red `af4e31d`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Pool.ReconcileMesh` | пустая дельта → 0 касаний и 0 refresh; payload → 1 бакет, тот же компонент, 1 render refresh; collision → 1 physics refresh; descriptor → новый ISM на pool-акторе, старый уничтожен, второй меш нетронут, число бакетов прежнее, хэндлы/lookup/transform живы, скрытый owner остаётся скрытым и `ShowOwner` попадает в новый бакет, новый `Add` с тем же дескриптором попадает в мигрированный бакет; чужой меш → 0 |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`af4e31d`) | `R5B0_RED_TEST.log`: `ReconcileMesh` Fail (0 buckets, 0 touched) |
| GREEN non-unity/no-PCH build | `R5B0_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.Pool` | `R5B0_GREEN_TEST.log`: 3/0 |
| полный NullRHI suite | `R5B0_GREEN_FULL.log`: `Success=208 Fail=0` (207 + 1) |
| `BuildPlugin -StrictIncludes` | `R5B0_STRICT.log`: ExitCode=0 (Success) |
| force-unity | `R5B0_FORCE_UNITY.log`: Succeeded |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public/Composite/MHInstancePool.h`,
`Private/Composite/MHInstancePool.cpp`. Tests: `MHInstancePoolTest.cpp`.
Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. R5b-1 (следующий, близнец): компилятор материализует static-листья
через `Add` пула, строки `FMHCompositeLeafMaterialization` получают хэндл,
`ReconcileEndpoint` актора делегирует меш-дельты `ReconcileMesh` (один раз на
мир из `MHNotifyGeneratedResourceChanged`), Outliner резолвит выбор через
`ReverseLookup`, `PostEditUndo`/`Destroyed` → `RemoveOwner` + rebuild из записи
актора (OPEN-R-1), скрытие актора в редакторе → `SetOwnerEditorVisibility`.
