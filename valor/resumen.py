# Mediana por método/patrón de p50, p99 y GB/s sobre las corridas registradas en valor/resultados-*.txt
import glob, re, statistics as S, collections
rows = collections.defaultdict(list)
for f in sorted(glob.glob("valor/resultados-host-de-pruebas-*.txt")):
    for l in open(f):
        if l.startswith("RES"):
            d = dict(kv.split("=") for kv in l.split()[1:])
            rows[(d["patron"], d["metodo"])].append(d)
print("| patrón | método | n | p50 ms (mediana) | p99 ms (mediana) | p99 ms (rango) | GB/s (mediana) | errores |")
print("|---|---|---|---|---|---|---|---|")
base = {}
for pat in ("aleat", "secu"):
    for m in ("swap", "tiermem", "mmap", "explicit"):
        r = rows.get((pat, m), [])
        if not r: continue
        p50 = S.median(float(x["p50_ms"]) for x in r); p99s = [float(x["p99_ms"]) for x in r]; p99 = S.median(p99s)
        gb = S.median(float(x["GBps"]) for x in r); err = sum(int(x["errores"]) for x in r)
        if m == "swap": base[pat] = p99
        print(f"| {pat} | {m} | {len(r)} | {p50:.1f} | {p99:.1f} | {min(p99s):.1f}–{max(p99s):.1f} | {gb:.3f} | {err} |")
for pat, b in base.items():
    t = S.median(float(x["p99_ms"]) for x in rows[(pat, "tiermem")])
    print(f"\n{pat}: p99 tiermem / p99 swap = {t:.1f}/{b:.1f} = {t/b:.3f}x (umbral <= 0.50x) -> {'PASA' if t/b <= 0.5 else 'NO PASA'}")
