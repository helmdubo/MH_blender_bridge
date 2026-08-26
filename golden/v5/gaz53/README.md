# GAZ-53 v5 freeze fixture

Это topology-only fixture V5-S0 из трёх protocol-файлов `.composite`.
`gaz53_b_random_cmp` включает body composite и random composite; random
содержит три ordered mesh options с weight 1. Body содержит parent-local probe
`100 + 25 = 125`.

Option resource tokens `gaz53_body_bc_option_0..2` явно synthetic: owner не
передал production GAZ tokens/Dagor source/profile values (`OPEN-V5-6`).
Fixture не является GAZ content authority.

Expected choices, raw draws, sampled transforms и ResolvedSignature намеренно
отсутствуют до owner-решений `OPEN-V5-1`/`OPEN-V5-3`. S0 не угадывает RNG.
