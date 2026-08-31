# Квитанция — канонизация донорского scale-шума (14 transform-блокировок)

Дата: 2026-08-31
Ветка: `fix/donor-scale-noise`
Исполнитель среза: Lead (по слову owner «сделай сам», ратификация варианта A)
Статус: **READY FOR OWNER FIELD TEST (переэкспорт + Import Changed)**

## 1. Полевой дефект

14 композитов cottage (кухни, спальни, гостиные) блокировались UE-импортом
с `MH_E_UNREPRESENTABLE_TRANSFORM: ... cannot round-trip through FTransform
within 8 ULP` по composed-world цепочкам.

## 2. Диагноз

- Каждый узел по отдельности валиден; отказ давала только композиция
  parent×child, которую проверяет лишь UE-резолвер на полном дереве
  (Blender-адаптер видит один документ; вложенные композиты конвертируются
  от Identity).
- Виновник во всех цепочках один: родительские узлы со scale вида
  `(1.0212899, 1.0212300, 1.0266)` — микроразница X≠Y ~6e-5 — и повёрнутые
  вокруг Z дети (5°..142.5°). Z-анизотропия с yaw коммутирует и shear не
  даёт; весь shear (метрика 1e-5..6e-5 > 8-ULP порога ~1e-6) рождается из
  X≠Y-шума.
- Скан сцены показал: шум системный (десятки объектов с тем же паттерном,
  сотни с аналогичным) — это накопленная float-ошибка дагоровских матриц
  round-trip'а CDK, а не авторство. Править данные руками невозможно.

## 3. Решение owner (2026-08-31, вариант A)

Dagor-файлы — донорские; авторитет — mh4blend-сцена и наши canonical-файлы.
На границе dag4blend-адаптера (`_canonical_local_transform`, оба пути:
прямой `.blk`-импорт и сценовый direct-export) компоненты scale, попарно
различающиеся менее чем на **2e-4 относительно**, выравниваются к среднему.
Порог разделяет шум (~6e-8..8e-5) и авторскую анизотропию (≥5e-3) с запасом
на порядок в обе стороны. Сценовый путь репортит
`MH_W_SCALE_NOISE_CANONICALIZED` (новый W-код в ОБОИХ реестрах:
`canonical.py::ERROR_CODES` и `MHDiagnosticRegistry.cpp`, теперь 53E/16W).
Wire-формат, 8-ULP предикат, резолвер и подписи не менялись; MH-родные
сцены через границу не проходят. Нормативный текст — docs/10 §6.2.

## 4. Red → green

| Тест | RED | GREEN |
|---|---|---|
| `test_donor_scale_noise_is_canonicalized_with_warning` | failed | pass |
| `test_authored_scale_anisotropy_is_never_touched` | — (сразу зелёный: поведение и было верным) | pass |
| Реестровые счётчики (pure `test_canonical`, UE `StreamTraceAndSignatureParity`) | 15W→16W | pass |

## 5. Гейты

| Гейт | Результат |
|---|---|
| Pure | **313 passed / 14 skipped** |
| Blender-hosted, 12 модулей | **369/369, 0 failed** |
| Guarded UE build | **Succeeded** |
| Полный NullRHI + `-MHGoldenRoot` | **162/162, 0 failed** |
| Полевое staging-репро cottage_i | **732/732 мешей; kitchen_a children[2] scale = (1.02127, 1.02127, 1.0266)** — X==Y канонизирован, авторский Z нетронут |

`golden/`, `reference/` не тронуты.

## 6. Полевой протокол owner

1. Перезапустить Blender (аддон 0.11.0), UE-плагин переустановлен
   (изменился только реестр диагностик + тесты).
2. `Export Composite All Stuff` — canonical-байты затронутых композитов
   изменятся (чистые scale); в отчёте появятся
   `MH_W_SCALE_NOISE_CANONICALIZED` warnings.
3. UE Import Changed — ожидание: все 14 `MH_E_UNREPRESENTABLE_TRANSFORM`
   блокировок уходят; кухни/спальни/гостиные и главный
   `sovmod_cottage_i_cmp` собираются.
