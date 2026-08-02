import csv,collections,statistics
rows=list(csv.reader(open('/tmp/fadesweep.csv')))
# snr,config,channel,mod,rate,ci,seed,n,sync_fail,decode_fail,crc_fail,pass
agg=collections.defaultdict(list)
for r in rows:
    if len(r)<12: continue
    snr=int(r[0]); mod=r[3]; rate=r[4]; n=int(r[7]); ok=int(r[11])
    agg[(mod,rate,snr)].append(ok/n)
# Historical FER-derived delivered-rate estimate.  Keep the original scheduling
# assumption so old sweeps remain comparable, but use PRODUCTION carrier geometry:
# 59 occupied carriers with spacing-8 pilots leaves 51 data carriers.  The former
# CAR=59 silently counted all eight pilots as payload and overstated every result by
# 59/51 = 15.7%.
eta={('qpsk','r3_4'):2*0.75,('qam8','r2_3'):3*2/3,('qam8','r3_4'):3*0.75,('qam16','r2_3'):4*2/3}
TOTAL_CARRIERS=59
PILOT_SPACING=8
DATA_CARRIERS=TOTAL_CARRIERS-((TOTAL_CARRIERS+PILOT_SPACING-1)//PILOT_SPACING)
SYM=48000/1152
SCHED=0.659*0.90
print(f"geometry: {TOTAL_CARRIERS} occupied - {TOTAL_CARRIERS-DATA_CARRIERS} pilots = {DATA_CARRIERS} data carriers; historical scheduling={SCHED:.4f}")
print(f"{'rung':<14}{'SNR':>5}{'FER':>8}{'pass rate':>11}{'eff. delivered bps':>20}")
best={}
for (mod,rate,snr),v in sorted(agg.items(), key=lambda x:(x[0][0],x[0][1],x[0][2])):
    pr=statistics.mean(v); fer=1-pr
    raw=eta[(mod,rate)]*DATA_CARRIERS*SYM
    # throughput with ARQ: delivered ~ raw * pass_rate * scheduling
    dl=raw*pr*SCHED
    name=f"{mod} {rate}"
    print(f"{name:<14}{snr:>5}{fer:>8.1%}{pr:>11.1%}{dl:>20.0f}")
    best.setdefault(name,[]).append((snr,fer,dl))
print()
print("FLOOR = lowest SNR with FER <= 10% (the convention QPSK anchors use is floor + 2 dB margin)")
for name,rowsx in best.items():
    fl=[s for s,f,_ in rowsx if f<=0.10]
    print(f"  {name:<14} floor {min(fl) if fl else '>24'}  -> Good anchor would be {min(fl)+2 if fl else 'n/a'}")
