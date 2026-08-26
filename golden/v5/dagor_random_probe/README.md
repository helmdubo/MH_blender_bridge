# Dagor Random Parity Probe

`random_parity_probe.composit.blk` — минимальный probe с тремя ordered
composite-options равного веса и всеми transform ranges. Команда

```text
python tools/dagor_random_parity_probe.py
```

детерминированно создаёт `source_derived_vectors.json` и GAZ baseline. В JSON
строго разделены:

- нормативные байты `mh.random_stream:1`;
- факты, выведенные из публичного Dagor source, закреплённого exact commit;
- необязательный исторический шаблон runtime-наблюдения Dagor со статусом
  `not_run_not_a_gate`.

При runtime-прогоне три `mh_probe_*_cmp` должны существовать как пустые
composite assets, иначе Dagor editor отфильтрует отсутствующие references ещё
до random selection.

Source-derived probe уже доказывает различие PRNG primitives и фиксирует
selection-векторы. Он не притворяется запуском Dagor. В pinned C++ axis-binding
transform draws зависит от порядка вычисления изменяющих seed аргументов;
поэтому JSON хранит две иллюстративные, не исчерпывающие source-derived
кандидатуры. Owner выбрал вариант B: совместимость поведенческая, Dagor не
участвует в shipping-пути, а placement seed назначается заново в UE. Поэтому
`runtime_observation.template.json` сохранён только как необязательная
историческая форма и больше не является gate. Байты `mh.random_stream:1`
окончательны и не меняются.
