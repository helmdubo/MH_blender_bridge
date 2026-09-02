# Полевой тест owner после R2a (2026-09-02)

Статус: **PASSED (owner)**. Установка: портфолио
`D:\PersonalProjects\UE5\MimirHead_portfolio 5.7`, плагин — плоская копия
`origin/main` `4782082` (после R2a, `Plugins\MimirComposite\INSTALLED_COMMIT.txt`),
`MimirHead_portfolioEditor` собран с нуля (старые Binaries/Intermediate плагина
удалены). Предыдущая установленная версия была до программы (без M0/R0/R2a),
поэтому это первый полевой прогон всей линии D0a → R2a и S0/S2.

Формат: пользовательский (удобство, скорость, интерфейс, взаимодействие), не
замер по протоколу M0. Числа ниже — из логов owner'а.

## 1. Оценка owner

> Всё нравится, стало гораздо быстрей обновляться, реимпорт меша работает
> корректно, композиты обновляются быстрей. Оба Random seed работают очень
> быстро, почти как в Dagor. Undo работает.

## 2. Счётчики из логов

### Reseed / duplicate композита (239 option-композитов, 732 уникальных меша)

`MH_PERF_MAPLOAD`: `registry_lookups=0 asset_registry_tag_queries=0
package_loads_sync=0 identity_admissions=0 live_receipt_tag_reads=0
waited_meshes=0 wait_static_mesh_compilation_ms=0.000` — цели R0/R1 выполнены
в поле.

| Поле | Значение |
|---|---|
| `selected_unique_meshes` | 333 / 331 |
| `resolve_composite_plan_ms` | 40.27 |
| `compile_placement_ms` | 151.1 / 149.6 |
| `components_created / reused / destroyed` | 333 / 1 / 333 |
| `ism_buckets / ism_instances` | 333 / 870 |
| `total_ms` | 206.6 / 205.5 |

### Реимпорт меша (S0)

| Меш | `full_scan_count_delta` | `incremental_paths` | `analysis_services_ms` | `import_build_ms` | `projection_ms` | `notified_actors` | `actor_rebuild_ms_total` | `total_ms` |
|---|---|---|---|---|---|---|---|---|
| `sovmod_cottage_i_wall` | 0 | 1 | 1408 | 1274 | 1215 | 2 | 310 | 4089 |
| `sovmod_cottage_i_exterior_decor` | 0 | 1 | 653 | 601 | 1391 | 4 | 547 | 2946 |

Полного скана нет; реимпорт FBX «работает очень быстро» (owner).

## 3. Находки (не дефекты R2a; вход для следующих срезов)

1. **Undo перемещения композита — ~15.7 с** (`Undo Move Elements`,
   `Ignoring very large delta of 15.73 seconds`). Причина по коду:
   plan-view компоненты создаются с `RF_Transactional`
   (`MHCompositePlacementCompiler.cpp`, `PlanViewFlags`), и на 333 ISM-компонента
   / 870 инстансов транзакция «Move Elements» пишет и восстанавливает каждый; а
   `AMHCompositeActor::PostEditUndo` делает полный `RebuildComposite`. Это ровно
   OPEN-R-1 / docs/16 §2.8 («транзакционен только актор; пул восстанавливает
   материализацию в `PostEditUndo`») — закрывается R4 (пулы/хэндлы), частично
   R2b (актор без provenance, materialize без rebuild).
2. **Reseed = полный rebuild**: `components_created=333, reused=1,
   destroyed=333` на каждую смену сида/дубликат. Ожидаемо до R4 (diff
   add/update/remove по хэндлам, docs/16 §4).
3. **`projection_ms` 1.2–1.4 с** на реимпорт одного меша — крупнейшая доля
   `total_ms` после анализа/импорта. Стоит замерить отдельно в S-линии
   (projection индекса), вне программы R.

## 4. Решения owner по итогам

- OPEN-R2A-1 закрыт: preview не использует `SelectedDependencies`; shadow
  parity сравнивает поле только по ресурсам графа (docs/16 §2.3, §9). Код:
  `MHCompareRecipeShadowParity` фильтрует ключи `composite:/static_mesh:/
  placement_profile:/actor:`.
- Программа продолжается: R2b (близнец).
