# R2c — точки выхода proof-плоскости

Статус: **STOP — OPEN-R2C-1**. Реализация и тестовые гейты не начаты: контракт
одновременно требует два новых теста исполнителя и запрещает исполнителю менять
тесты, а тестовый файл отсутствует в закрытом списке.

## 1. База и границы

- ветка: `recipe/r2c-exit-points`; HEAD контракта `78fc4a4`; база
  `origin/main` `ec1d5ff`; red-коммит близнеца `94938d7`;
- отдельный worktree:
  `E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge_r2c_executor`;
- обязательные документы прочитаны в порядке задания; трекер подтверждает
  `R2c | NEXT`;
- production-код, публичный API, red-тест, трекер, Engine, audit-host и
  portfolio-проект не изменялись; `main` не переключался, не обновлялся и не
  пушился.

## 2. Имеющийся red

Контракт фиксирует red-коммит `94938d7` и лог близнеца
`E:\MimirComposite_R_M0_20260902\Saved\Logs\R2C_RED2_TEST.log`:

```text
1077: Test Completed. Result={Fail} Name={BuildPreflightFullClosure}
```

Исполнитель не выдаёт этот лог за собственный red-first прогон. Собственная
сборка и первый red-прогон не запускались, потому что обязательный объём
Acceptance нельзя выполнить в пределах закрытого списка.

## 3. OPEN-вопросы

### OPEN-R2C-1 — два обязательных теста вне закрытого списка

- **Контекст:** Acceptance 2 требует новый тест
  `Mimir.V5.Composite.Proof.SaveWarnsWithoutProof`, Acceptance 3 — новый тест
  `Mimir.V5.Composite.Proof.StaleSourceBlocksCookAndSnapshot`. В ветке и
  red-коммите есть только
  `Mimir.V5.Composite.Proof.BuildPreflightFullClosure` в
  `MimirCompositeTests/Private/MHProofCacheTest.cpp`; поиск двух требуемых имён
  по test module даёт ноль вхождений. При этом `MHProofCacheTest.cpp` не включён
  в закрытый список, а следующий абзац контракта прямо говорит «Тесты не
  менять».
- **Вопрос:** близнец добавляет оба red-теста отдельным коммитом и оставляет их
  неизменяемыми для исполнителя, либо контракт расширяет закрытый список на
  `MimirCompositeTests/Private/MHProofCacheTest.cpp` и явно разрешает
  исполнителю добавить ровно эти два теста?
- **Временное fail-closed правило:** тесты, API и production-код не менять;
  собственный red/green, полный suite и build-гейты не начинать; трекер оставить
  `NEXT`; PR в `main` не открывать, пока объём не станет непротиворечивым.
- **Статус:** **OPEN — нужен нормативный ответ близнеца/owner.**

## 4. Гейты

| Gate | Результат |
|---|---|
| собственный non-unity/no-PCH build | NOT RUN — STOP до реализации |
| собственный red-first | NOT RUN — OPEN-R2C-1 |
| Acceptance 1–5 | NOT RUN — закрытый список не позволяет выполнить Acceptance 2–3 |
| force-unity | NOT RUN — STOP |
| `BuildPlugin -StrictIncludes` | NOT RUN — STOP |
| полный NullRHI `Automation RunTests Mimir` | NOT RUN — STOP |
| `RecipeShadowParityTest` | NOT RUN — STOP |
| `git diff --check` | PASS для STOP-квитанции |
| `python tools/check_normative_docs.py` | PASS для STOP-квитанции |

## 5. Изменённые файлы

- `docs/receipts/recipe_r2c.md` — эта STOP-квитанция, файл разрешён закрытым
  списком.

Тесты не менялись и не удалялись. Других изменений нет.

## 6. Строка трекера и PR

Трекер не изменён: `R2c | NEXT`. PR не открыт по временному fail-closed правилу
OPEN-R2C-1.
