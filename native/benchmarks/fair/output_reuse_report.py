"""Audit and retain every strict FFN tile screen, including rejected variants."""
import hashlib,json,statistics as st
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];OUT=ROOT/'benchmarks/results'
variants=['screen','spaced','quad','limit-register','limit-adjacent','limit-spaced','limit-quad','rows','leaf32','leaf128','leaf32-repeat']
labels=['Adjacent columns, 16×32','Spaced columns, 16×32','Four outputs, 32×32','Original kernel, limit 256','Adjacent columns, limit 256','Spaced columns, limit 256','Four outputs, limit 256','Two rows, 32×16','32-leaf chunks','128-leaf chunks','32-leaf repeat (contended)']
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
qualification=[];summary=[];evidence={}
for name,label in zip(variants,labels):
 file=OUT/f'gpu-output-reuse-{name}-2026-10-29.json';data=json.loads(file.read_text());evidence[file.name]=sha(file)
 build=ROOT/'benchmarks/.build/gpu-output-reuse-2026-10-29'/('leaf32' if name=='leaf32-repeat' else name)
 qfile=build/'qualification.json';q=json.loads(qfile.read_text());assert len(q['checks'])==72 and all(r['passed'] for r in q['checks'])
 for item in (data,q):
  for collection in ('source_sha256','artifact_sha256'):
   for path,digest in item[collection].items():assert sha(ROOT/path)==digest,path
 if 'qualification_sha256' in data:assert data['qualification_sha256']==sha(qfile)
 blocks=7 if name=='leaf32-repeat' else 5
 assert data['protocol']['blocks']==blocks
 grouped={}
 for row in data['pairs']:grouped.setdefault(tuple(row['group']),[]).append(row['time_reduction']*100)
 assert len(grouped)==6 and all(len(v)==blocks for v in grouped.values())
 summary.append(dict(variant=name,label=label,block_count=blocks,shapes=[dict(m=m,k=k,n=n,reduction_percent=st.median(values),positive_blocks=sum(v>0 for v in values),blocks=values) for (m,k,n),values in sorted(grouped.items())]))
 if name!='leaf32-repeat':qualification.append(dict(variant=name,evidence=q,evidence_sha256=sha(qfile)))
qout=OUT/'gpu-output-reuse-qualification-2026-10-29.json';qout.write_text(json.dumps(dict(checks=720,failed=0,records=qualification),indent=2)+'\n');evidence[qout.name]=sha(qout)
shapes=[(16,512,512),(16,512,2048),(16,2048,512),(64,512,512),(64,512,2048),(64,2048,512)]
lines=['# Strict FFN tile experiments', '',
 'Apple M2 Max, 29 October 2026. **No production change is accepted.** All ten controls pass their numerical/guard qualification, but none provides a broad improvement over the existing register16 kernel. These are isolated matrix screens; the previous whole-transformer acceptance remains the current language result.', '',
 'The initial adjacent-column tile is 16.74–51.43% slower across six shapes. Preserving contiguous lane accesses does not rescue it. A four-output tile is dramatically slower. Explicitly limiting compilation to the 256 threads actually dispatched leaves the original shader nearly flat and does not rescue the larger tiles. Register spills were not measured, so neither a spill nor an occupancy diagnosis is proven by these timings.', '',
 'Smaller 32-leaf chunks are 7.97–11.91% slower in the initial run and 7.73–11.84% slower in a seven-block repeat. Both encountered external CPU spikes (583% and 602% across cores), so these are retained as contended diagnostics, not clean acceptance or a final rejection. Larger 128-leaf chunks improve two small cells by 4.60% and 5.17%, while regressing other cells by up to 42.92%; these wins remain visible but do not justify replacing the production policy. The wider FFN expansion and contraction—the target of this experiment—do not improve.', '',
 '## All paired outcomes', '',
 'Positive percentages mean less elapsed time. Parentheses show positive blocks out of five, or seven in the repeat. Each cell uses adjacent ABBA/BAAB fresh processes, two warmups and three measured batches; adaptive batches target 30 ms with a 256-invocation cap. The faster side determines the shared count. No outlier removal is applied.', '',
 '| Variant | 16×512×512 | 16×512×2048 | 16×2048×512 | 64×512×512 | 64×512×2048 | 64×2048×512 |',
 '|---|---:|---:|---:|---:|---:|---:|']
for row in summary:
 cells={(c['m'],c['k'],c['n']):c for c in row['shapes']}
 lines.append('| '+row['label']+' | '+' | '.join(f"{cells[s]['reduction_percent']:+.2f}% ({cells[s]['positive_blocks']}/{row['block_count']})" for s in shapes)+' |')
lines += ['', '## Qualification and scope', '',
 'There are 720 passing case executions: ten controls × twelve shapes × three input patterns × baseline/candidate. Every warmup and measured batch checks every output bit against the original GPU kernel, input immutability, and output guard bytes outside timing. Non-dyadic mixed-scale and signed-zero inputs additionally use a separately rounded CPU adjacent-pair oracle. IEEE-pattern inputs use bit equality to the existing GPU behavior; that pattern includes NaNs, infinities and subnormals and is not a new independent IEEE conformance proof.', '',
 'The 128-leaf experiment keeps the original entry when K is not divisible by 128. Qualification includes those inactive cases, odd output tails and non-power-of-two contraction lengths. New variants are appended to copied shaders, and copied drivers select matching dispatch geometry. Production source generation, fusion, runtime ownership, cancellation, admission and default policy are unchanged. No whole-program speedup or new cancellation qualification is claimed from an isolated kernel screen.', '',
 'One command buffer executes repeated matrix calls against preallocated private buffers. Invocation lifecycle, allocation, input uploads and readback are excluded from timed batches. CPU reference generation and readbacks occur between batches; this is not a steady application workload. These screens cannot replace end-to-end closed/cancellable transformer acceptance.', '',
 '## Pipeline controls and load', '',
 'The unvalidated original pipeline reports a 1,024-thread maximum and 8 KiB static threadgroup storage. Explicit controls report a 256-thread maximum; two-output variants use 12 KiB, four-output variants 16 KiB. Every dispatch still uses 256 threads. Validation instrumentation changes reported resource values, so those values are retained separately rather than treated as production resource counts.', '',
 'The thread-limit control follows [Apple’s documented pipeline property](https://developer.apple.com/documentation/metal/mtlcomputepipelinedescriptor/maxtotalthreadsperthreadgroup). Apple also discusses the interaction of register pressure, threadgroup storage and compilation limits in [Metal Compute on MacBook Pro](https://developer.apple.com/videos/play/tech-talks/10580/). Here, changing that limit is not a demonstrated optimization. It does not measure register spills or prove that occupancy caused the regressions.', '',
 'All builds and validation completed before the corresponding sequential timing series. No profiling or other local test/build jobs overlap these timings. Ordinary background applications remain active. The initial screen has only one late load snapshot; later series retain periodic external-load samples. These are not dedicated idle-machine measurements.', '',
 '## Next decision', '',
 'Keep register16 and its 64-leaf chunks. The 32-leaf control still needs a quiet confirmation before a final verdict. Before another tile policy, obtain direct evidence about register usage, spills and active waves on representative FFN shapes. Alternatively screen batched Q/K/V dispatch as an independent source of overhead, with full numerical and task-lifetime qualification. A future shape-specific 128-leaf policy would need longer independent replication, inactive-shape controls and whole-block/upload acceptance; its isolated small-cell gains are insufficient here. SIMD-group matrix instructions, FMA or reassociation require a separate explicit numerical contract, not an implicit replacement of strict arithmetic.', '',
 '## Evidence', '']
for name in variants:
 filename=f'gpu-output-reuse-{name}-2026-10-29.json';lines.append(f'- [{name}]({filename})')
 for suffix in ('load',):
  f=OUT/f'gpu-output-reuse-{name}-{suffix}-2026-10-29.json'
  if f.exists():evidence[f.name]=sha(f);lines.append(f'  - [External load]({f.name})')
resources=OUT/'gpu-output-reuse-pipeline-resources-2026-10-29.json';evidence[resources.name]=sha(resources)
lines += [f'- [All qualification]({qout.name})',f'- [Pipeline properties]({resources.name})']
base=OUT/'gpu-output-reuse-2026-10-29';base.with_suffix('.md').write_text('\n'.join(lines)+'\n')
base.with_suffix('.json').write_text(json.dumps(dict(production_change=False,checks=720,variants=summary,evidence_sha256=evidence),indent=2)+'\n')
print(base.with_suffix('.md'))
