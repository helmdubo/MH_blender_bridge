# Composite loading A/B и пауза после Save — 2026-09-07

Статус: ACCEPTED / READY FOR FIELD TEST. Owner принял предложенные срезы A/B и
добавил устранение зависания после Save. Ветка `codex/composite-loading`,
база `c6faeae`. Исследование: `docs/reference_notes/composite_loading_vs_packed_level_actor_20260907.md`.

## Принятое поведение

1. A: normal placement вычисляет выбранный план один раз. Пока выбранные
   mesh packages загружаются, новый placement не создаёт кубы на листьях;
   прежнее представление при обновлении сохраняется до готовности замены.
   Outliner показывает загрузку. Невыбранные варианты не запрашиваются.
2. Завершение загрузки имеет отдельное уведомление с группировкой событий;
   оно не является реимпортом, не инвалидирует proof и не повторяет resolver.
   Готовые зависимости удерживаются до передачи компонентам. Смена seed,
   определения или удаление актора не допускают применения устаревшего плана.
3. B: актор сохраняет дедуплицированные hard references выбранных мешей как
   editor-only dependency hints. Обычный package loader UE видит их при
   повторном открытии сцены. Это не авторитетный план, signature или snapshot
   источника: source definition, call context и seeds остаются входами.
   Старые карты без hints проходят путь A и получают hints при следующем
   обычном сохранении. Изменение hints при успешном commit помечает пакет
   dirty, включая завершение загрузки после Save; неизменный набор не помечает.
   ISM pool и его instances остаются transient.
4. Обычный Save Map выполняет только чтение proof cache и предупреждения.
   Он не ставит Unknown placements в очередь RequestProof. Явный proof,
   preflight, cook и runtime snapshot сохраняют проверки. Это заменяет
   прежнее требование R2c планировать proof после сохранения.
5. Ready UObject не означает готовность всех shader resources/texture mips.
   Missing/Invalid остаются диагностикой и не маскируются как Loading.

## Связанные изменения и проверки

Замена прежнего R4 теста кубов и R2c теста отложенного proof прямо следует
из принятого owner поведения; старые квитанции остаются историей.
Production scope: Actor, EndpointPrototypeRegistry, Outliner, ProofCache,
EditProjection (подписка на новый readiness, чтобы cold mesh в Edit тоже
обновлялся). Tests: AsyncEndpoint, MeshDependencies, ProofCache, EditLoadReady.
B использует тот же
commit point актора, что A, поэтому эти два изменения проверяются совместно.

Red-first: cold selected meshes, несколько завершений, старый вид до готовности,
нулевое уведомление реимпорта, отсутствие proof jobs после Save; для B —
selected-only refs и настоящий save/unload/reload. Затем полный NullRHI Mimir,
ShadowParity, guarded non-unity/no-PCH, StrictIncludes, force-unity adaptive-off,
изолированный RHI smoke и perf traces. Полевое время owner не заменяется
синтетическими замерами. Установка только проверенной сборки при закрытом UE.
