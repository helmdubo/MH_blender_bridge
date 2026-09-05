# R5b-1a (Recipe Model v2.1) — дрейфнувший бакет пула не переиспользуется

Статус: **REVIEW** (близнец). Мини-срез между R5b-0 и R5b-1: перенос в пул
инварианта компилятора «бакет, чьё живое состояние разошлось с ключом, не
переиспользуется» (`Mimir.V5.Composite.ISM.BucketPolicyRejectsMutatedReuse`).
Без него R5b-1 падал: тест мутировал collision на общем компоненте пула
синтетического меша без body setup, следующий `AddInstance` — assert
`BodySetup` в `InstancedStaticMesh.cpp:2841` (`R5B1_GREEN_FULL.log`).

## 1. Что сделано

- `UMHInstancePoolSubsystem::FindOrCreateBucket`: найденный бакет проверяется
  `PoolLiveMatchesDescriptor(component, descriptor)` — живые поля компонента
  (меш, override-материалы, collision profile/enabled/object type/responses,
  overlap, trace complex, return material, cast shadow, distance field, ray
  tracing, mobility, visibility, hidden in game, `NumCustomDataFloats`) против
  дескриптора. Расхождение → `MigrateBucket` (R5b-0): свежий компонент из
  дескриптора, инстансы и хэндлы переезжают, старый уничтожается, метрика
  `BucketsMigrated`. Add без дрейфа ничего не мигрирует.
- `PoolConfigureBucket` восстанавливает именованный collision profile после
  сеттеров (иначе живой профиль всегда «Custom» и сравнение зацикливает
  миграции) — то же, что делал старый `MHMigrateCompositePlacementBucket`.

## 2. Тесты (red `4acf1e5`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Pool.DriftedBucketIsRetired` | два Add без дрейфа → 0 миграций; `CastShadow` перевёрнут на общем компоненте → следующий Add: новый компонент по дескриптору, старый уничтожен, 1 бакет, 3 инстанса, хэндлы/transform/lookup живы; `SetStaticMesh(другой меш)` → снова миграция (компонент рендерит меш дескриптора), 4 инстанса; следующий Add без дрейфа — без миграции |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`4acf1e5`) | `R5B1A_RED_TEST.log`: `DriftedBucketIsRetired` Fail |
| GREEN non-unity/no-PCH build | `R5B1A_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.Pool` | `R5B1A_GREEN_TEST.log`: 4/0 |
| полный NullRHI suite | `R5B1A_GREEN_FULL.log`: `Success=209 Fail=0` (208 + 1) |
| `BuildPlugin -StrictIncludes` | `R5B1A_STRICT.log`: ExitCode=0 (Success) |
| force-unity | `R5B1A_FORCE_UNITY.log`: Succeeded |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Private/Composite/MHInstancePool.cpp`. Tests:
`MHInstancePoolTest.cpp`. Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта
квитанция.

## 5. Вопросы

Открытых нет.
