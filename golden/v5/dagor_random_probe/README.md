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
- реальное runtime-наблюдение Dagor, которое пока имеет статус `not_run`.

При runtime-прогоне три `mh_probe_*_cmp` должны существовать как пустые
composite assets, иначе Dagor editor отфильтрует отсутствующие references ещё
до random selection.

Source-derived probe уже доказывает различие PRNG primitives и фиксирует
selection-векторы. Он не притворяется запуском Dagor. В pinned C++ axis-binding
transform draws зависит от порядка вычисления изменяющих seed аргументов;
поэтому JSON хранит две иллюстративные, не исчерпывающие source-derived
кандидатуры, а
`runtime_observation.template.json` требует фактический executable/version/
command/output. Заполненный observation и owner-решение A/B остаются gate перед
V5-S2; байты `mh.random_stream:1` этим срезом не меняются.
