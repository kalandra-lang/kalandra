#!/usr/bin/env python3
"""Render the depth study with Matplotlib: INPUT.json OUTPUT_STEM."""
import json,pathlib,statistics,sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
src=pathlib.Path(sys.argv[1])
p=json.loads(src.read_text());destination=pathlib.Path(sys.argv[2])
bg='#0b0b0f';fg='#f1f0f7';muted='#aaa8b8';grid='#2c2935';colors={'cpp':'#b19aff','rust':'#e7ab7f'}
plt.rcParams.update({'font.family':'DejaVu Sans','text.color':fg,'axes.labelcolor':muted,'xtick.color':muted,'ytick.color':muted,'font.size':12,'svg.fonttype':'none'})
fig,axes=plt.subplots(2,2,figsize=(12,9.5),facecolor=bg)
fig.subplots_adjust(left=.095,right=.965,top=.78,bottom=.19,hspace=.45,wspace=.2)
fig.text(.055,.946,'Recursion scaling beyond depth 14',fontsize=24,fontweight='bold')
fig.text(.055,.902,'Experimental cancellable Kalandra / ordinary C++ and Rust time · lower is faster',fontsize=13,color=muted)
fig.legend(handles=[Line2D([0],[0],color=colors[l],marker=m,linewidth=2,label=f'Kalandra / {label}') for l,m,label in [('cpp','o','C++'),('rust','s','Rust')]],loc='upper left',bbox_to_anchor=(.047,.869),ncol=2,frameon=False,fontsize=13)
for kind,ax in enumerate(axes.flat):
 ax.set_facecolor(bg)
 ds=[100,1000,10000] if kind==3 else [14,16,18,20,22]
 for lang,marker in [('cpp','o'),('rust','s')]:
  ys=[statistics.median(1/r['speedup'] for r in p['pairs'] if r['group']==[lang,kind,d]) for d in ds]
  ax.plot(ds,ys,color=colors[lang],marker=marker,linewidth=2,markersize=6,zorder=3)
 ax.axhline(1,color='#8b859b',linewidth=1,zorder=1)
 ax.axhline(1.1,color='#8b859b',linewidth=.8,linestyle=(0,(2,4)),zorder=1)
 ax.set_title(['Binary tree · polynomial','Binary tree · branches','Binary tree · Euclid / GCD','Non-tail chain · linear work'][kind],fontsize=14,fontweight='bold',loc='left',pad=13)
 if kind==3:
  ax.set_xscale('log');ax.set_xticks(ds,['100','1,000','10,000']);ax.minorticks_off();ax.set_xlim(80,13000)
  ax.set_xlabel('Call depth (log scale)',fontsize=12,labelpad=9)
  ax.annotate('+27% vs C++\n+25% vs Rust',xy=(100,1.27),xytext=(210,1.29),fontsize=11.5,color=fg,va='center',arrowprops={'arrowstyle':'-','color':muted,'linewidth':.7})
 else:
  ax.set_xticks(ds);ax.set_xlim(13.6,22.4);ax.set_xlabel('Tree depth',fontsize=12,labelpad=9)
 ax.set_ylim(.59,1.41);ax.set_yticks([.6,.8,1,1.2,1.4],['0.6×','0.8×','1.0×','1.2×','1.4×'])
 ax.grid(axis='y',color=grid,linewidth=.6,zorder=0);ax.tick_params(length=0,pad=8)
 for spine in ax.spines.values():spine.set_visible(False)
fig.text(.055,.115,'Solid horizontal: parity. Dotted horizontal: 10% slower. Points: median of three paired blocks.',fontsize=11.5,color=muted)
fig.text(.055,.078,'Depth 22 trees have 4,194,304 leaves. The chain has one child per level and real work after every return.',fontsize=11.5,color=muted)
fig.text(.055,.041,'CPU only · Apple M2 Max / macOS 27 · Rational-reference checks passed · 25 Oct 2026',fontsize=11.5,color=muted)
fig.savefig(destination.with_suffix(".png"),dpi=200,facecolor=bg)
fig.savefig(destination.with_suffix(".svg"),facecolor=bg)
svg=destination.with_suffix(".svg")
svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()).rstrip()+"\n")
