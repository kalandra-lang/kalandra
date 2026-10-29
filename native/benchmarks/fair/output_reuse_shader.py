"""Experimental strict 16x32 matrix tile; only append to copied shader sources."""
import re


def function(source, prefix):
    start = source.index(prefix)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def append_pair32(source):
    names = [name for name in ('hardware', 'fused')
             if 'kernel void kalandra_matmul_' + name + '_register16(' in source]
    if not names:
        return source
    helper = function(source, 'inline float kalandra_hardware_eight16(')
    helper = helper.replace('inline float ', 'inline float2 ', 1).replace('eight16(', 'eight_pair32(')
    helper = re.sub(r'float p([0-7])=', r'float2 p\1=', helper)
    helper, count = re.subn(r'b\[\(first\+([0-7])\)\*16\+col\]',
        lambda m: f'float2(b[(first+{m[1]})*32+col],b[(first+{m[1]})*32+col+1])', helper)
    assert count == 8
    result = source + '\n' + helper + '\n'
    for name in names:
        kernel = function(source, f'kernel void kalandra_matmul_{name}_register16(')
        kernel = kernel.replace(f'kalandra_matmul_{name}_register16(', f'kalandra_matmul_{name}_pair32(')
        kernel = kernel.replace('av[1024],bv[1024]', 'av[1024],bv[2048]')
        kernel = kernel.replace('tileColumns=n/16+uint(n%16!=0)', 'tileColumns=n/32+uint(n%32!=0)')
        kernel = kernel.replace('colBase=(group%tileColumns)*16', 'colBase=(group%tileColumns)*32')
        kernel = kernel.replace('col=colBase+lane%16', 'col=colBase+(lane%16)*2')
        old = '''      const uint r=rowBase+at/64,c=colBase+at%16;
      av[at]=r<m ? a.values[r*k+first+at%64] : 0.0f;
      bv[at]=c<n ? b.values[(first+at/16)*n+c] : 0.0f;'''
        new = '''      const uint r=rowBase+at/64;
      av[at]=r<m ? a.values[r*k+first+at%64] : 0.0f;
    }
    for(uint at=lane;at<2048;at+=256) {
      const uint c=colBase+at%32;
      bv[at]=c<n ? b.values[(first+at/32)*n+c] : 0.0f;'''
        assert kernel.count(old) == 1
        kernel = kernel.replace(old, new)
        kernel = kernel.replace('float s0=', 'float2 s0=')
        kernel = re.sub(r'float r([0-7])=', r'float2 r\1=', kernel)
        kernel = kernel.replace('kalandra_hardware_eight16(av,bv,lane/16,lane%16,',
                                'kalandra_hardware_eight_pair32(av,bv,lane/16,(lane%16)*2,')
        kernel = kernel.replace('float value=', 'float2 value=')
        store = re.search(r'    output.values\[row\*n\+col\]=([^;]+);', kernel)
        assert store
        expression = store[1]
        replacement = '    output.values[row*n+col]=' + re.sub(r'\bvalue\b', 'value.x', expression) + ';\n'
        replacement += '    if(col+1<n) output.values[row*n+col+1]=' + re.sub(r'\bcol\b', '(col+1)',
            re.sub(r'\bvalue\b', 'value.y', expression)) + ';'
        kernel = kernel[:store.start()] + replacement + kernel[store.end():]
        result += kernel + '\n'
    return result
