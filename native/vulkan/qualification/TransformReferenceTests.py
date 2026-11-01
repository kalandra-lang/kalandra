"""Checks that the native precision oracle accepts bounds and rejects corruption."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tests'))
from TensorTransformReference import D, to_bits, from_bits
from TransformReference import ulp, envelope, divide, sqrt, reference

class OracleTests(unittest.TestCase):
    def test_spacing(self):
        self.assertEqual(ulp(D(1)),D(2)**-24)
        self.assertEqual(ulp(D('1.5')),D(2)**-23)
        self.assertEqual(ulp(D(0)),D(2)**-149)
    def test_error_interval_has_no_free_ulp(self):
        for x in (D(1),D('1.1234567'),D(-1),D('-1.1234567')):
            for count in (D(0),D(2),D('2.5')):
                low,high=envelope(x,count*ulp(x))
                if count==0 and low>high:continue # No representable exact result.
                self.assertGreaterEqual(low,x-count*ulp(x))
                self.assertLessEqual(high,x+count*ulp(x))
    def test_division_bound(self):
        lo,hi=divide((D(1),D(1)),(D(3),D(3)))
        self.assertTrue(lo<=D.from_float(1/3)<=hi)
        self.assertFalse(lo<=D('0.333')<=hi)
        with self.assertRaises(AssertionError):divide((D(1),D(1)),(D(2)**-130,D(2)**-130))
    def test_native_rms_difference_and_corruption(self):
        bounds=reference([to_bits(-3,'f','I')],1,1,2,D.from_float(from_bits(0x3727c5ac,'f','I')),'f','I')
        lo,hi=bounds[0]
        self.assertTrue(lo<=D.from_float(from_bits(0xbf7ffff8,'f','I'))<=hi)
        self.assertTrue(lo<=D.from_float(from_bits(0xbf7ffff7,'f','I'))<=hi)
        self.assertFalse(lo<=D('-0.999')<=hi)
        self.assertLess(hi-lo,D('0.000003'))
    def test_softmax_sum_and_wrong_algorithm(self):
        bits=[to_bits(x,'f','I') for x in (-2,0,3)]
        bounds=reference(bits,1,3,1,D('0.00001'),'f','I')
        self.assertTrue(sum(x[0] for x in bounds)<=1<=sum(x[1] for x in bounds))
        self.assertFalse(all(lo<=D(1)/3<=hi for lo,hi in bounds))
    def test_transpose_payload(self):
        bits=[0x7f800001,0x80000000,1,0x7fc12345,0xff800000,0x3f800000]
        self.assertEqual(reference(bits,2,3,0,None,'f','I'),[bits[i] for i in (0,3,1,4,2,5)])
    def test_positive_infinity_indicator_division(self):
        bounds=reference([0x7f800000]*7,1,7,1,None,'f','I')
        for lo,hi in bounds:
            self.assertTrue(lo<=D(1)/7<=hi)
            self.assertFalse(lo<=D('0.14')<=hi)
    def test_nan_contract(self):
        for op in (1,2):
            got=reference([0x7fc12345,0x3f800000],1,2,op,D('0.00001'),'f','I')
            self.assertTrue(all(lo.is_nan() and hi.is_nan() for lo,hi in got))

if __name__=='__main__':unittest.main()
