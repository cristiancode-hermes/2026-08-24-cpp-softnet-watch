# softnet-watch — architecture

```
/proc/net/softnet_stat   (or --file / --demo)
            |
      slurp + skip '#'
            |
   parse_softnet_text()
   hex fields → CpuRow
   13-col vs 15-col layout
            |
        Snapshot + summarize()
            |
  +---------+--------------+
  |         |              |
table/KPI  JSON        watch loop
 (ANSI)  (quoted keys) (delta + rate)
```

- CPU id: campo 12 (15-col) o campo 11 (13-col); fallback = índice de línea.
- Primer frame de `--watch` no calcula tasas; el siguiente usa `dt = now - prev`
  (si `dt <= 0` o es el primer ciclo con prev, fallback `--watch N`).
- Alert = dropped | squeeze | backlog | flow_limit | input_qlen | process_qlen.
