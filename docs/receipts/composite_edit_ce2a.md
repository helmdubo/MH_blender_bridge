# CE-2a (Composite Edit Mode) — suppression lease в пуле инстансов

Статус: **REVIEW** (близнец). Первая половина карточки CE-2 спецификации
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md` §6.1);
проекция — CE-2b.

## 1. Что сделано

- `UMHInstancePoolSubsystem::AcquireSuppression(handles)` →
  `FMHPoolSuppressionLease {LeaseId, Handles}`, `ReleaseSuppression(lease)`,
  `IsSuppressed(handle)`. У слота две независимые оси: `bOwnerHidden`
  (Hide/ShowOwner) и `SuppressionCount` (сессии); `bHidden` — эффективное
  «не в ISM» = owner-hidden ∨ count > 0, пересчитывается одним местом
  (`ApplySlotVisibility`), которое и двигает инстанс в/из компонента.
- `HideOwner`/`ShowOwner` переписаны на owner-ось: показ владельца не
  снимает suppression. `Add`/`Remove` сбрасывают обе оси. `MigrateBucket`,
  `NumLiveInstances`, `GetInstance`, `ReverseLookup` работают по
  эффективному `bHidden` — не менялись.
- Release: неизвестный/повторный lease — no-op (`ActiveLeases`); мёртвый
  хэндл (слот удалён/переиспользован, другая generation) пропускается —
  новый инстанс в этом слоте не трогается.
- Fixture CE-0 отдаёт `MeshAssetA/C` для reconcile-тестов.

## 2. Тесты (red `7e9bf76`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Suppression.LeaseHidesOnlyItsInstances` | lease на строки второго вызова A: хэндлы валидны, `IsSuppressed`, `GetInstance` → INDEX_NONE, live-счётчик уменьшился ровно на N, вызов не рендерится; первый вызов, B и чужой ISM не тронуты; `HideOwner` → 0, `ShowOwner` → lease в силе; release восстанавливает позиции; повторный release — no-op |
| `…Suppression.StaleReleaseTouchesNothing` | после `RebuildComposite` владельца (слоты переизданы) старый хэндл мёртв, всё видно; release старого lease ничего не меняет |
| `…Suppression.MigrationKeepsTheLease` | `ReconcileMesh(bBucketDescriptor)` мигрирует бакеты mesh C: lease сохранён, хэндлы живы и подавлены, соседний видимый лист резолвится через `ReverseLookup` к владельцу; release после миграции возвращает инстансы |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`7e9bf76`) | `CE2A_RED_TEST.log`: Fail ×3 |
| GREEN non-unity/no-PCH build | `CE2A_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite` (группа) | `CE2A_GREEN_TEST.log`: 135/0 |
| полный NullRHI suite | `CE2A_FULL.log`: `Success=243 Fail=0 (240 + 3)` |
| force-unity | `CE2A_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE2A_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public|Private/Composite/MHInstancePool.{h,cpp}`. Tests:
`MHCompositeEditSuppressionTest.cpp` (новый), `MHCompositeEditFixture.h`.
Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Следующий — CE-2b: проекция редактируемого определения
(компоненты одного transient projection-актора), флаг `bCompositeEditModeV2`,
локальный preview выбранного occurrence через lease.
