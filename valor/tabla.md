| patrón | método | n | p50 ms (mediana) | p99 ms (mediana) | p99 ms (rango) | GB/s (mediana) | errores |
|---|---|---|---|---|---|---|---|
| aleat | swap | 5 | 60.2 | 809.6 | 141.7–1009.0 | 0.012 | 0 |
| aleat | tiermem | 5 | 13.6 | 26.5 | 23.1–41.0 | 0.082 | 0 |
| aleat | mmap | 5 | 35.9 | 100.2 | 58.4–106.2 | 0.026 | 0 |
| aleat | explicit | 5 | 10.8 | 26.1 | 20.5–31.3 | 0.094 | 0 |
| secu | swap | 2 | 59.4 | 212.1 | 147.4–276.9 | 0.015 | 0 |
| secu | tiermem | 2 | 0.7 | 77.2 | 71.9–82.4 | 0.096 | 0 |
| secu | mmap | 2 | 36.9 | 73.0 | 70.4–75.7 | 0.029 | 0 |
| secu | explicit | 2 | 4.5 | 22.0 | 16.9–27.1 | 0.158 | 0 |

aleat: p99 tiermem / p99 swap = 26.5/809.6 = 0.033x (umbral <= 0.50x) -> PASA

secu: p99 tiermem / p99 swap = 77.2/212.1 = 0.364x (umbral <= 0.50x) -> PASA
