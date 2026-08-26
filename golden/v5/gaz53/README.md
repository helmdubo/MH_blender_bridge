# GAZ-53 v5 fixture

Три owner-provided `*.composit.blk` теперь привязаны как source oracle. Root
`gaz53_b_random_cmp` включает body и random composite; random содержит реальные
ordered composite-options `gaz53_bread_b_cmp`, `gaz53_wooden_b_cmp`,
`gaz53_wooden_c_cmp` с implicit weight 1. Synthetic mesh tokens V5-S0 удалены.
Body protocol-fixture сохраняет отдельный parent-local probe `100 + 25 = 125`.

`baseline_rng_choices.json` фиксирует raw selection draw, option index и token
для seed set по нормативному `mh.random_stream:1`. Это MH-норматив, а не
наблюдение Dagor runtime. Тот же artifact извлекает из всех трёх BLK полный
declared-reference frontier: семь composite identities и шестнадцать mesh
resource identities, включая невыбранные random options.

Полный GAZ closure и `ResolvedSignature` пока fail-closed не вычисляются:
предоставленные три файла задают только frontier, но payloads referenced
composites/meshes и их дальнейших material/texture dependencies в oracle
отсутствуют. Реальное Dagor runtime-наблюдение также не подменяется
source-derived моделью; его статус и provenance описаны в соседнем
`golden/v5/dagor_random_probe/`.
