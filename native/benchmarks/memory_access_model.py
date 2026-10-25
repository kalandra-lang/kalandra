"""Illustrative address footprints; buckets are analytical units, not hardware counters."""
import argparse,json
from pathlib import Path

def tile(group,rows,columns,grouped):
    if not grouped:return group//columns,group%columns
    span=16*columns;first=(group//span)*16;height=min(16,rows-first);within=group%span
    return first+within%height,within//height

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();assert not a.output.exists()
    # Exhaustively prove the ID permutation across short/full/tail row groups.
    cases=0;visited=0
    for rows in range(1,131):
        for columns in [1,2,3,7,16,31,32,63,64,65,127,128,129]:
            seen={tile(i,rows,columns,True) for i in range(rows*columns)}
            assert len(seen)==rows*columns and all(0<=r<rows and 0<=c<columns for r,c in seen)
            cases+=1;visited+=rows*columns
    records=[]
    for m,k,n in [(1024,1024,1024),(2048,2048,2048),(1024,4096,1024),(1025,576,1027)]:
        tr=(m+15)//16;tc=(n+15)//16
        for grouped in [False,True]:
            for storage in ['baseline','block','both']:
                buckets={size:set() for size in [4,128,4096]};requests=0
                for group in range(min(32,tr*tc)):
                    rr,cc=tile(group,tr,tc,grouped)
                    for at in range(1024):
                        r=rr*16+at//64;c=cc*16+at%16
                        if r<m:
                            index=(rr*(k//64))*1024+at if storage=='both' else r*k+at%64
                            for size,seen in buckets.items():seen.add(('a',index*4//size))
                            requests+=4
                        inner=at//16
                        if c<n:
                            index=cc*1024+at if storage=='block' else cc*(k//64)*1024+at if storage=='both' else inner*n+c
                            for size,seen in buckets.items():seen.add(('b',index*4//size))
                            requests+=4
                records.append(dict(shape=[m,k,n],grouped=grouped,storage=storage,logical_requested_bytes=requests,unique_useful_bytes=len(buckets[4])*4,unique_128byte_buckets=len(buckets[128]),unique_4KiB_regions=len(buckets[4096])))
    a.output.write_text(json.dumps(dict(contract='Model of first K=64 panel in the first 32 logical workgroup IDs. Physical execution ordering, cache geometry, cache misses, DRAM traffic and timing are NOT modeled or measured. 128-byte buckets and 4-KiB regions are illustrative address units, not asserted hardware cache/page sizes. No cross-panel reuse or compiler register effects. Exhaustive tile permutation validation is separate from performance.',permutation_shapes=cases,permutation_tiles=visited,records=records,model_source=Path(__file__).read_text()),indent=2)+'\n')
if __name__=='__main__':main()
