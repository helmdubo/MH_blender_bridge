# R5a (Recipe Model v2.1) — `UMHInstancePoolSubsystem`: сервис пула со стабильными хэндлами

Статус: **MERGED** (близнец, #111 `c8495ad`). Первая половина R5 по KICKOFF §5 / docs/16 §2.8:
сам сервис пула и его инварианты. R5b (отдельный срез) переводит на него
материализацию `AMHCompositeActor`, Outliner (`ReverseLookup`) и
восстановление после Undo (OPEN-R-1).

## 1. Что сделано

- `Public/Composite/MHInstancePool.h`, `Private/Composite/MHInstancePool.cpp`
  (новые): `UMHInstancePoolSubsystem` (`UWorldSubsystem`, только Editor /
  EditorPreview миры), `AMHInstancePoolActor` (transient, editor-only, не в
  Outliner) — один на `ULevel`, `FMHPoolBucketDescriptor` (полная идентичность
  бакета: те же поля, что у приватного ключа компилятора размещений, плюс
  `AppearanceLayout` и `AppearanceCustomDataBaseIndex`; `FromMesh` повторяет
  `PlanViewDefaultBucketKey`), `FMHInstanceHandle { BucketId, SlotId,
  Generation }`, `FMHInstancePoolMetrics`.
- Бакет = `{ULevel, дескриптор}` → один ISM-компонент на pool-акторе уровня,
  сконфигурированный как бакеты компилятора (`bHasPerInstanceHitProxies`,
  `NumCustomDataFloats = AppearanceLayout`), но с `bSupportRemoveAtSwap = true`:
  `Remove` — swap-remove, карты `SlotId → ISM index` и `ISM index → SlotId`
  обновляются вместе, хэндлы остальных не меняются. Освобождённый слот
  переиспользуется со следующей `Generation`, старый хэндл мёртв.
- API по §2.8: `Add/Update/UpdateAppearance/Remove/IsValidHandle`,
  `ReverseLookup(Component, ISMIndex) → (Owner, NodePath)`, `GetInstance`,
  `BeginBulk/EndBulk` (один `MarkRenderStateDirty` и один bounds-refresh на
  затронутый бакет за скоуп; вне скоупа — на операцию), owner-операции
  `HideOwner/ShowOwner/RemoveOwner/MoveOwner/SetOwnerEditorVisibility`.
  Hide = инстанс убирается из ISM, слот и хэндл живы (`InstanceIndex =
  INDEX_NONE`), Show возвращает его с сохранённой world-матрицей и каналами
  appearance. `(Component*, InstanceIndex)` наружу не выдаётся как идентичность.
- Никто из существующего кода на пул ещё не переведён: компилятор размещений,
  Outliner и Undo работают как до среза (R5b).

## 2. Тесты (red `09f9e55`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Pool.HandleStability` | совместимые инстансы двух owner'ов делят один бакет, другой меш — свой; swap-remove среднего инстанса: хэндлы и `ReverseLookup` выживших верны, transform сохранён; удалённый хэндл отвергается `IsValidHandle/Update`; reuse слота даёт новую generation; `Update` двигает инстанс |
| `Mimir.V5.Composite.Pool.OwnerOperations` | `HideOwner` убирает только инстансы owner'а (ISM-компонент видим для второго), хэндлы живы, `NumLiveInstances == 0`; `ShowOwner` восстанавливает transform и lookup; `MoveOwner` двигает только своего; bulk-скоуп: 0 refresh внутри, ровно 1 на бакет в `EndBulk`; `RemoveOwner` освобождает слоты, чужой инстанс жив |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`09f9e55`) | `R5A_RED_TEST.log`: оба теста Fail (handles unset, 0 buckets) |
| GREEN non-unity/no-PCH build | `R5A_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.Pool` | `R5A_GREEN_TEST.log`: 2/0 |
| полный NullRHI suite | `R5A_GREEN_FULL.log`: `Success=207 Fail=0` (205 + 2) |
| `BuildPlugin -StrictIncludes` | `R5A_STRICT.log`: ExitCode=0 (Success) |
| force-unity | `R5A_FORCE_UNITY.log`: Succeeded |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public/Composite/MHInstancePool.h`,
`Private/Composite/MHInstancePool.cpp` (новые). Tests:
`MHInstancePoolTest.cpp` (новый). Docs: `docs/16_recipe_model.md` §2.8
(статус R5a/R5b), `docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Замечания на R5b: `AddInstance` движка сам помечает render
state — счётчик `RenderStateRefreshes` считает только refresh'и пула; пакетный
`AddInstances` для холодного построения — по замеру R5b. Домен пула —
`ULevel`; WP-cell и Data Layers — после полевого теста (OPEN-R-5).
