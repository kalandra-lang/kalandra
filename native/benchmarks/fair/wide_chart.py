"""Render the committed six-process CPU comparison; no benchmark runs required."""
import json
from pathlib import Path
import statistics
import math
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

ROOT=Path(__file__).resolve().parents[3]
RESULTS=ROOT/'benchmarks/results'
FILES=[RESULTS/'wide-input-final-2026-10-28.json',RESULTS/'wide-input-final-repeat-2026-10-28.json']
samples={}
for path in FILES:
    for row in json.loads(path.read_text())['summary']:
        samples.setdefault(tuple(row['cell']),[]).extend(row['process_medians_ns'])
assert len(samples)==162 and all(len(v)==6 for v in samples.values())
times={k:statistics.median(v) for k,v in samples.items()}
groups=sorted({k[1:] for k in times})
within=sum(times[('kalandra',*k)]<=1.1*min(times[('cpp',*k)],times[('rust',*k)]) for k in groups)
wins=sum(times[('kalandra',*k)]<min(times[('cpp',*k)],times[('rust',*k)]) for k in groups)
worst=max(times[('kalandra',*k)]/min(times[('cpp',*k)],times[('rust',*k)])-1 for k in groups)
assert within==54 and wins==29
names=[('affine','Affine recurrence'),('polynomial','Polynomial'),('euclid','Euclid / GCD'),('euclid_mixed','Changing-state GCD'),('array32','Array map / sum · 32'),('array128','Array map / sum · 128')]
colors={'kalandra':'#B69BFF','cpp':'#929BAA','rust':'#E8AC83'}
bg='#0b0a0f';fg='#efedf3';muted='#a3a0ae'
plt.rcParams.update({'font.family':'DejaVu Sans','font.size':11,'text.color':fg,'axes.labelcolor':muted,'xtick.color':muted,'ytick.color':fg,'svg.fonttype':'none'})
fig,axes=plt.subplots(1,3,figsize=(18,10),sharey=True)
fig.patch.set_facecolor(bg)
fig.subplots_adjust(left=.18,right=.98,bottom=.25,top=.75,wspace=.17)
fig.text(.055,.93,'Kalandra vs. C++ and Rust',fontsize=28,weight='bold')
fig.text(.055,.884,'Equivalent CPU source algorithms · rotating signed / wide inputs · C++ time = 1.00 · lower is faster',fontsize=13,color=muted)
legend=[Patch(color=colors[k],label=v) for k,v in [('kalandra','Kalandra'),('cpp','C++'),('rust','Rust')]]
fig.legend(handles=legend,loc='upper left',bbox_to_anchor=(.055,.854),ncol=3,frameon=False,fontsize=13,handlelength=1.3,columnspacing=2.4)
def label(ns):
    return f'{ns/1000:,.2f} µs' if ns>=1000 else f'{ns:.2f} ns'
plotted=[]
for ax,count in zip(axes,[1,16,4096]):
    ax.set_facecolor(bg)
    ax.set_title(f'{count:,} '+('round' if count==1 else 'rounds'),fontsize=16,weight='bold',pad=20)
    for index,(name,title) in enumerate(names):
        cpp=times[('cpp',name,'rotating',count)]
        for offset,lang in zip([-.24,0,.24],['kalandra','cpp','rust']):
            ns=times[(lang,name,'rotating',count)];ratio=ns/cpp
            y=index+offset
            ax.barh(y,ratio,height=.195,color=colors[lang],zorder=3)
            ax.text(ratio+.027,y,label(ns),va='center',fontsize=9.5)
            plotted.append(dict(language=lang,program=name,regime='rotating',rounds=count,ns=ns,relative_to_cpp=ratio))
    ax.set_xlim(0,1.62);ax.set_ylim(5.6,-.6)
    ax.set_xticks([0,.5,1,1.5]);ax.set_xticklabels(['0','0.5','1.0','1.5'])
    ax.axvline(1,color='#77717f',ls=(0,(3,4)),lw=1,zorder=2)
    ax.grid(axis='x',color='#28252f',lw=.7,zorder=0)
    ax.set_yticks(range(6));ax.set_yticklabels([title for _,title in names],fontsize=12)
    ax.tick_params(axis='both',length=0,pad=10)
    for spine in ax.spines.values():spine.set_visible(False)
    ax.set_xlabel('Relative execution time',labelpad=16)
fig.text(.055,.157,f'Full 54-case corpus: {within}/54 within 10% of the faster reference · {wins}/54 faster than both · worst gap <{math.ceil(worst*1000)/10:.1f}%',fontsize=13,weight='bold')
fig.text(.055,.115,'Chart shows all 18 rotating-input cases. Labels: elapsed time per call; bars: median of six process medians across two complete runs.',fontsize=10.8,color=muted)
fig.text(.055,.081,'Exact per-call verification · strict floating arithmetic · ordinary CPU execution · no numerical libraries. These are measured workloads, not a language-wide ranking.',fontsize=10.8,color=muted)
fig.text(.055,.047,'Apple M2 Max · macOS 27.2 · 28 Oct 2026 · Clang 23.1.3 / Rust 1.93.0 · compiler a7e82604, committed evidence 87a456fd',fontsize=10.8,color=muted)
base=RESULTS/'cpu-comparison-2026-10-28'
fig.savefig(base.with_suffix('.png'),dpi=180,facecolor=bg)
fig.savefig(base.with_suffix('.svg'),facecolor=bg)
base.with_suffix('.json').write_text(json.dumps(dict(source_files=[str(p.relative_to(ROOT)) for p in FILES],aggregation='Median of six independent process medians per language/case, across both full runs',within_10_percent=within,faster_than_both=wins,worst_gap=worst,plotted=plotted),indent=2)+'\n')
print(base.with_suffix('.png'))
