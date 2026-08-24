# softnet-watch

Monitor per-CPU de NAPI / softnet RX del kernel Linux en tiempo real (C++23, zero deps).

Lee `/proc/net/softnet_stat` (una línea hex por CPU) para mostrar paquetes procesados,
drops de backlog, `time_squeeze` (cuota de `net_rx_action` agotada), RPS, flow-limit y
longitud de cola. Útil para detectar CPUs saturadas en RX.

## Features

- **Parse de `/proc/net/softnet_stat`**: 13 columnas (kernel 6.8) y 15 columnas (6.8+)
- **Health por CPU**: OK / WARN (squeeze, flow-limit) / HOT (drops, backlog)
- **Watch mode**: refresco periódico con delta + rate (`pkt/s`) entre snapshots
- **JSON output**: `summary` + array `cpus` validable
- **Demo mode**: dump sintético de 8 CPUs con drops/squeeze/backlog
- **Filtros**: `--cpu N`, `--alerts`, `--sort processed|dropped|squeeze|backlog|rate`
- **`--file`**: parsea un dump (tolera comentarios `#`)
- **`--no-color`**: paleta ANSI mutable (0 códigos en pipes)
- **Selftest**: 58 checks (parser hex, layouts 13/15, filtros, JSON, `/proc` real)

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o softnet-watch
```

## Usage

```bash
# Snapshot único (host real)
./softnet-watch

# Modo watch (cada 2s) con tasas
./softnet-watch --watch 2

# Solo CPUs con drops / squeeze / backlog
./softnet-watch --alerts

# Una CPU, ordenado por drops
./softnet-watch --cpu 1 --sort dropped

# JSON
./softnet-watch --json

# Demo (dump sintético 8 CPUs)
./softnet-watch --demo

# Parsear un dump con comentarios
./softnet-watch --file sample-softnet.table --no-color

# Sin colores
./softnet-watch --no-color

# Auto-tests
./softnet-watch --selftest
```

## Data Source

| Archivo | Contenido |
|---------|-----------|
| `/proc/net/softnet_stat` | 1 línea hex por CPU (NAPI RX) |

Columnas (kernel 6.8+):

| # | Campo | Significado |
|---|--------|-------------|
| 0 | processed | paquetes NAPI procesados |
| 1 | dropped | paquetes tirados (backlog lleno) |
| 2 | time_squeeze | `net_rx_action` agotó cuota/tiempo |
| 8 | received_rps | wakeups RPS hacia esta CPU |
| 9 | flow_limit | hits de flow-limit |
| 10 | backlog | longitud actual de backlog |
| 12/11 | cpu | índice de CPU (15-col / 13-col) |
| 13/12 | input_qlen | cola de input |
| 14 | process_qlen | cola de process (solo 15-col) |

## Example Output

```
+================================================+
|   Linux softnet / NAPI per-CPU RX Monitor    |
+================================================+
  Source: /proc/net/softnet_stat

  -- KPIs --
  processed      205.91M
  dropped        30
  squeeze        46.1k
  alerts         7
  hottest CPU    cpu3

  -- Per-CPU NAPI --
   cpu  processed  dropped  squeeze    rps   flow   back    inq  health
     0     45.23M        0    12.7k      0      0      0      0   WARN
     1     46.49M       18    12.8k      4      0      0      0    HOT
     2     27.33M        0     4227      0      0      3      2    HOT
```

## License

MIT
