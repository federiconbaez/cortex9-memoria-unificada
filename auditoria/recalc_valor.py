import re,glob,statistics as S,random,collections
rows=collections.defaultdict(list)
for f in ["valor/resultados-host-de-pruebas-reps1y2.txt","valor/resultados-host-de-pruebas-aleat-reps3a5.txt"]:
    for l in open(f):
        if l.startswith("RES"):
            d=dict(kv.split("=") for kv in l.split()[1:]); rows[(d["patron"],d["metodo"])].append(d)
for k,v in rows.items():
    p=[float(x["p99_ms"]) for x in v]
    print(k,len(v),"p99s",p,"med",S.median(p),"swapMiB",[x["swap_cgroup_MiB"] for x in v],"err",sum(int(x["errores"]) for x in v),"total_s",[x["total_s"] for x in v])
for pat in ("aleat","secu"):
    s=[float(x["p99_ms"]) for x in rows[(pat,"swap")]]; t=[float(x["p99_ms"]) for x in rows[(pat,"tiermem")]]
    print(pat,"mediana ratio",S.median(t)/S.median(s),"pareado",[round(a/b,4) for a,b in zip(t,s)],"peor/mejor",max(t)/min(s))
    random.seed(1); B=[]
    for _ in range(100000):
        rs=[random.choice(s) for _ in s]; rt=[random.choice(t) for _ in t]; B.append(S.median(rt)/S.median(rs))
    B.sort(); print(pat,"bootstrap mediana-ratio IC95",B[2500],B[97500],"P(>0.5)",sum(b>0.5 for b in B)/len(B))
