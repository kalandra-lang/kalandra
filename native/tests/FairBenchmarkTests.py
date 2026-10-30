"""Integrity tests for independently checked benchmark evidence."""
import json
import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'benchmarks/fair'))
import protocol as p
import library_reference as ref
import pipeline_reference as pipeline
import source
import source_run

class EvidenceTests(unittest.TestCase):
    def test_rectangular_pipeline_reference(self):
        f32=lambda x:struct.unpack('f',struct.pack('f',x))[0]
        for rows,inner,columns in [(1,32,7),(2,544,33),(3,17,5),(1,16352,3)]:
            a=[(i%17-8)/8 for i in range(rows*inner)]
            b=[(i%13-6)/8 for i in range(inner*columns)]
            cells=[]
            for r in range(rows):
                for c in range(columns):
                    products=[f32(a[r*inner+k]*b[k*columns+c]) for k in range(inner)]
                    while len(products)>1:
                        products=[f32(products[i]+products[i+1]) for i in range(0,len(products)-1,2)]+(products[-1:] if len(products)&1 else [])
                    cells.append(max(0.,f32(f32(products[0]+.125)*.25)))
            while len(cells)>1:
                cells=[f32(cells[i]+cells[i+1]) for i in range(0,len(cells)-1,2)]+(cells[-1:] if len(cells)&1 else [])
            self.assertEqual(pipeline.exact_rect(rows,inner,columns),cells[0])

    def test_pipeline_fixed_point_reference(self):
        f32=lambda x:struct.unpack('f',struct.pack('f',x))[0]
        for value in (2**24-1,2**24+1,2**24+3,2**25+2,2**25+6,2**40+65536):
            self.assertEqual(pipeline.round32_integer(value),int(f32(value)))
        for n in (1,3,7,17):
            a,b=ref.inputs(n,2,'float32',0)
            cells=[max(0.,f32(f32(sum(a[r*n+k]*b[k*n+c] for k in range(n)))+.125)*.25)
                   for r in range(n) for c in range(n)]
            tree=cells.copy()
            while len(tree)>1:
                tree=[f32(tree[i]+tree[i+1]) for i in range(0,len(tree)-1,2)]+(tree[-1:] if len(tree)&1 else [])
            self.assertEqual(pipeline.exact(n),tree[0])
            pipeline.assess(n,0,tree[0],cells)
            bad=cells.copy();bad[0]+=1
            with self.assertRaises(AssertionError):pipeline.assess(n,0,tree[0],bad)

    def rows(self, value=4, ns=1):
        return '\n'.join(json.dumps(dict(round=r, checksum=value, ns=ns)) for r in (-2,-1,0))

    def test_reject_corrupt_and_missing_samples(self):
        for output in (self.rows(5), self.rows(ns=-1), self.rows(ns=float('nan')),
                       '\n'.join(self.rows().splitlines()[1:])):
            with self.assertRaises(AssertionError):
                p.observations(output, 2, 2, 1)
        rows=self.rows().splitlines()
        rows[0]=json.dumps(dict(round=-2, checksum=9, ns=1))
        with self.assertRaises(AssertionError):
            p.observations('\n'.join(rows), 2, 2, 1)

    def test_binary64_json_and_timer_resolution(self):
        # Go's shortest float64 representation can look like a JSON integer.
        value=195186167971027970
        p.observations(self.rows(value,0), float(value), 1, 1)
        cells=[['probe']]
        def invoke(cell,count,rounds):
            return [dict(round=r,ns=0,checksum=1) for r in range(-2,rounds)]
        with self.assertRaisesRegex(AssertionError,'timer resolution'):
            p.measure(cells,invoke,lambda c:1,processes=1,rounds=1)

    def test_source_closed_form_references(self):
        for kind,kernel in enumerate((source.polynomial,source.branches,source.euclid)):
            for depth in range(6):
                for seed in (1,17,31):
                    self.assertEqual(source_run.expected(kind,depth,seed),kernel(seed,depth))

    def test_exact_matrix_reference_against_cubic_products(self):
        for n in (1,3,17):
            a,b=ref.inputs(n,2,'float64',0)
            cells=[sum(a[r*n+k]*b[k*n+c] for k in range(n)) for r in range(n) for c in range(n)]
            self.assertEqual(ref.exact(2,n),sum(cells))
            ref.assess(2,n,'float64',0,sum(cells),cells)
            bad=cells.copy();bad[0]+=1
            with self.assertRaises(AssertionError):ref.assess(2,n,'float64',0,sum(cells),bad)
            with self.assertRaises(AssertionError):ref.assess(2,n,'float64',0,sum(cells)+1,cells)

    def test_cell_order_and_nonfinite_errors(self):
        a,b=ref.inputs(17,1,'float64',1)
        cells=[3*x+y for x,y in zip(a,b)]
        ref.assess(1,17,'float64',1,sum(cells),cells)
        for wrong in (cells[::-1],cells[:-1],[float('inf')]+cells[1:]):
            with self.assertRaises((AssertionError,ArithmeticError)):
                ref.assess(1,17,'float64',1,sum(cells),wrong)

if __name__=='__main__':unittest.main()
