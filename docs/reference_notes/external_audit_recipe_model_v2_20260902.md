> Status: REFERENCE · внешний аудит KICKOFF v2 (2026-09-02) · не норматив; решения Lead по нему — `docs/archive/D0b_kickoff_v2_1_amendments.md`. Аудитор писал по состоянию main до merge R0b/R1 (`a6d7059`); ссылки на «R0b NEXT» устарели.

# Внешний аудит KICKOFF v2 (второй аудит)

## Вердикт

Скорректированный KICKOFF v2 стал намного целостнее. Главное противоречие v1 устранено: отдельно существуют Preview plane (быстрый редакторский результат без freshness-proof и ожиданий), Proof plane (full closure, receipts, hashes, signatures на строгих границах), Source plane (индекс, файловая свежесть, targeted reimport). Формула: Dagor-подобный быстрый исполнитель в редакторе + строгая Mimir-проверка на границах.

Все четыре owner-решения принимаются; третье требует уточнения.

## Ответы на четыре решения

**1. Отложить NodeOverrides, физику и автосборку за R3.** Да; R6 после R5 правильнее, чем «после R3»: до R3 нестабильна семантика обновления endpoints, до R5 нет модели стабильных handles. Граница: Physics/Auto Assembly → FMHNodeOverrideSet transaction → MHMaterializeLayout. Ядро не знает, кто создал override.

**2. AppearanceSeed явный и не зависит от позиции.** Да. Placement transform changed → AppearanceSeed unchanged; AppearanceSeed changed → appearance changed, layout unchanged. bAutoAppearanceSeed = сгенерировать один раз при создании/дублировании, не пересчитывать из позиции. Position-derived — позднее как opt-in policy (Stored | Position Derived), не default.

**3. Authored local transform в fingerprint.** Да, собственный transform узла включать: override заменяет локальный transform, при изменении source transform старое абсолютное значение теряет смысл. Но: DisplayName исключить (presentation-only; хранить ExpectedLabel/CurrentLabel для диагностики). Parent fingerprint — только семантика родителя (kind, resource, structural role) без его authored transform, иначе сдвиг предка инвалидирует overrides всего поддерева; child override задан в локальном пространстве родителя и должен ехать с ним. Хэшировать canonical representation (знак кватерниона, -0.0→0.0, float-сериализация, порядок компонентов, запрет NaN/Inf), не raw bytes FTransform.

**4. Status headers + NORMATIVE_INDEX + CI.** Да. NORMATIVE / HISTORY / RECEIPT; лексический запрет только для удалённых code entities; ClosureHash, ResolvedSignature разрешены как proof-концепты; запрещено утверждение «ResolvedSignature определяет freshness placement actor».

## Что в KICKOFF v2 нужно поправить

1. **OPEN-R-6 закрыть.** MHResolveCompositePlan сначала вызывает MHBuildRandomSourceClosure, которое требует canonical raw hash каждого ресурса полного closure; Layout → Appearance → Proof обязателен, не условен.
2. **SeedAffectsResult == None** должно означать no-op (сохранить значение), а не appearance update. Категории: None — ничего; ChildSeedsOnly — только endpoints, потребляющие layout seed; Transform — пересчёт transforms + handles; Topology — layout + add/update/remove diff. AppearanceSeed обновляет только appearance channels.
3. **PlacementInterfaceHash слишком широкий** (bounds внутри означают, что любой geometry reimport считается сменой интерфейса). Разделить: PayloadRevision → render refresh; BoundsRevision → bounds cache; BucketDescriptorHash → bucket migration; CollisionInterfaceHash → physics state rebuild; MaterialBindingHash → material descriptor reconcile.
4. **PreSaveWorld не должен синхронно строить proof** (иначе stall переезжает с загрузки на Ctrl+S). Background Proof Cache (ключ: root recipe revision, source index generation, importer version, registry revision); PreSaveWorld только читает Fresh/Stale/Missing/ProofPending/Unknown. Синхронно ждать proof — только Build Preflight.
5. **Acceptance R1 без ≫**: waited_mesh_set == selected_compiling_mesh_set; waited ∩ unselected == ∅. Отношение all_option/selected — метрика, не условие.
6. **Execution tracker** docs/RECIPE_EXECUTION_STATUS.md; KICKOFF задаёт порядок, tracker — точку продолжения.

Дополнительно: ключ compiled recipe — UMHCompositeAsset* + RecipeRevision (или content hash), AppliedHash как debug-атрибут; статус docs/16 разделить на Document status: NORMATIVE / Target architecture rollout: TRANSITIONAL / Production cutover milestone: R2b.

## Итог

После этих точечных правок KICKOFF v2 — достаточно зрелый норматив для продолжения программы. Основная архитектурная развилка разрешена правильно.
