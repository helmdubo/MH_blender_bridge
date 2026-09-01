> HISTORY. Не норматив. Модель заменена docs/16_recipe_model.md (2026-09-02).

# ROADMAP — post-MVP направления с критериями пересмотра

> **SUPERSEDED BY SOURCE PROTOCOL V4 AS AN ACTIVE ROADMAP.** Перечень ниже
> сохранён как исторический backlog и не задаёт scope, приоритет или
> implementation requirements. Действующий норматив —
> [`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md), действующие
> срезы — [`09_v4_agent_slices.md`](09_v4_agent_slices.md).

Правило: направление не берётся в работу «по ощущению» — у каждого записаны
критерии, при выполнении которых решение пересматривается. Порядок внутри
списка — не приоритет.

## USD как geometry/transport backend

Замена FBX-транспорта (или параллельный путь) на USD.
Критерии пересмотра (все три):
- PointInstancer поддержан в UE Interchange;
- выбор parent-материала для USD-материалов доступен при импорте;
- USD-плагин UE вышел из beta.

Пока критерии не выполнены — FBX остаётся транспортом; схема bundle от
транспорта не зависит (D1: FBX — только payload, семантика в `.composite`).

## Interchange geometry backend (`FInterchangeGeometryBackend`, D24)

NodeContainer-pipeline вместо legacy FBX-импорта — только как реализация
`IGeometryImportBackend`. Средняя стадия (Analyzer/Ledger/diff) остаётся нашей
при любом backend'е. Метод приёмки: golden-сравнение меш-в-меш с legacy
(вершины/нормали/UV/секции/материальные слоты идентичны на golden-наборе).
Статус FBX-через-Interchange перепроверить на **UE 5.8** при рассмотрении.

## VariantSet + keyed random

Схема готова (kind, variants, seed_policy, формула §5). Реализация: реестр
каналов и hash-функция — по зафиксированной спеке; первый потребитель —
рулетка вариантов в компиляторе. Критерий старта: MVP-цикл стабилен на
реальном контенте (не golden).

## Break / Build New Composite

Порты `nodes_split` / `nodes_to_composite` (UX dag4blend). Требование D14:
Break даёт детям производные сиды (визуально нейтрально), Build New Composite
пишет `.composite` на диск и импортирует штатной фабрикой.

## ISM / Level Instance таргеты компиляции

По instance grouping key (D6). LI — только для детерминированных поддеревьев
без рандома в цепочке. Критерий старта: замеры на реальных уровнях
показывают, что StaticMeshComponent-разворот — узкое место.

## Adopt Existing (D16)

Привязка существующих uasset к UID. Обязательна ДО раскатки на реальный
проект — блокер миграции, не «когда-нибудь».

## registry.json UE → Blender (D17/D28)

Материальная часть уже в MVP (D28: генерация из master_root). Полный реестр
(placeable-классы, preview bounds, категории) — вместе с Actor/BP-узлами.

## CI-commandlet `-run=MHImportManifests` (D26)

Headless-импорт манифестов в CI. Критерий старта: появление CI-инфраструктуры
проекта (тогда же — сборочная матрица UE 5.7.4 + 5.8).
