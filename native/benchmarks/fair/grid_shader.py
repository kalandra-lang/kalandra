"""Clone strict register16 with a general 2D grid; optionally omit tile guards.

The aligned entry requires both output dimensions to be multiples of 16.
Contraction length, numerical operations, root tree and barriers are unchanged.
"""
from output_reuse_shader import function

def grid_kernel(source, entry, aligned=False):
    body = function(source, 'kernel void '+entry+'(')
    suffix = '_grid2d_aligned' if aligned else '_grid2d'
    replacements = {
        entry+'(': entry+suffix+'(',
        'uint lane [[thread_position_in_threadgroup]]': 'uint lane [[thread_index_in_threadgroup]]',
        'uint group [[threadgroup_position_in_grid]]': 'uint2 group [[threadgroup_position_in_grid]]',
        '  const uint tileColumns=n/16+uint(n%16!=0);\n': '',
        'const uint rowBase=(group/tileColumns)*16, colBase=(group%tileColumns)*16;':
            'const uint rowBase=group.y*16, colBase=group.x*16;',
    }
    for old,new in replacements.items():
        assert body.count(old)==1,old
        body=body.replace(old,new)
    if aligned:
        for old,new,count in [
            ('r<m ? a.values[r*k+first+at%64] : 0.0f','a.values[r*k+first+at%64]',1),
            ('c<n ? b.values[(first+at/16)*n+c] : 0.0f','b.values[(first+at/16)*n+c]',1),
            ('if(row<m && col<n) {','{',2),
        ]:
            assert body.count(old)==count,old
            body=body.replace(old,new)
    return body
