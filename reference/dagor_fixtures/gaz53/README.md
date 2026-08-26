# GAZ-53 Dagor oracle — ожидает owner-данных

Сюда owner кладёт три исходных Dagor composite-файла, которые становятся
authoritative oracle первого end-to-end golden v5 (10 §13.6):

```text
gaz53_b_random_cmp.composit.blk      # root: body + random variant
gaz53_b_body_cmp.composit.blk        # рекурсивный body composite
gaz53_body_bc_random_cmp.composit.blk # random node, три ent weight 1
```

До их появления `golden/v5/gaz53/` содержит topology-only фикстуру с явно
synthetic option-токенами: она проверяет грамматику, closure и parent-local
`100 + 25 = 125`, но НЕ участвует в RNG/parity acceptance.

Реальные опции — composite-ресурсы `gaz53_bread_b_cmp`, `gaz53_wooden_b_cmp`,
`gaz53_wooden_c_cmp` (не mesh); при появлении исходников V5-S1 заменяет
synthetic токены ими и фиксирует seed-векторы.

Файлы этого каталога — read-only reference: production-код их не читает.
