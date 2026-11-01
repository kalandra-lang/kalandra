"""Deterministic exit-race and ownership controls without touching real processes."""
import ctypes
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from editor_windows_processes import EditorProcessScope

class Kernel:
    def __init__(self, system, *, error=5, settles=True, image='wsl.exe'):
        self.system=system;self.error=error;self.settles=settles;self.image=image
        self.exited=False;self.waits=[];self.terminated=[];self.closed=[]
    def OpenProcess(self, rights, inherit, pid):
        assert rights==0x1000|0x100000|1 and not inherit and pid==7
        return 99
    def WaitForSingleObject(self, handle, timeout):
        assert handle==99
        self.waits.append(timeout)
        if timeout and self.settles:self.exited=True
        return 0 if self.exited else 258
    def QueryFullProcessImageNameW(self, handle, flags, image, length):
        assert handle==99 and flags==0
        image.value=str(self.system/self.image)
        return True
    def TerminateProcess(self, handle, code):
        assert handle==99 and code==0
        self.terminated.append(handle)
        return False
    def CloseHandle(self, handle):self.closed.append(handle)

class ProcessScopeTests(unittest.TestCase):
    def scope(self, root, **options):
        kernel=Kernel((root/'System32').resolve(),**options)
        scope=object.__new__(EditorProcessScope)
        scope.kernel=kernel;scope.job=None
        scope.children=lambda:[] if kernel.exited else [7]
        return scope,kernel

    def execute(self, **options):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            scope,kernel=self.scope(root,**options)
            with patch.dict(os.environ,SystemRoot=str(root)),patch.object(ctypes,'get_last_error',return_value=kernel.error,create=True),patch.object(ctypes,'WinError',side_effect=lambda value:OSError(value,'Windows process error'),create=True):
                scope.settle_helpers()
            return kernel

    def test_exit_race_waits_for_owned_handle_and_closes_it(self):
        kernel=self.execute()
        self.assertEqual(kernel.terminated,[99])
        self.assertTrue(kernel.exited)
        self.assertIn(5000,kernel.waits)
        self.assertEqual(kernel.closed,[99])

    def test_still_live_access_denial_is_fatal(self):
        with self.assertRaises(OSError) as caught:self.execute(settles=False)
        self.assertEqual(caught.exception.errno,5)

    def test_other_termination_errors_are_not_silenced(self):
        with self.assertRaises(OSError) as caught:self.execute(error=6)
        self.assertEqual(caught.exception.errno,6)

    def test_orphan_language_server_is_not_reaped(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);scope,kernel=self.scope(root,image='kalandra-lsp.exe')
            with patch.dict(os.environ,SystemRoot=str(root)):
                with self.assertRaisesRegex(RuntimeError,'Unexpected surviving editor child'):scope.settle_helpers()
            self.assertEqual(kernel.terminated,[])
            self.assertEqual(kernel.closed,[99])

    def test_surviving_job_descendant_still_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);scope,kernel=self.scope(root)
            scope.children=lambda:[7]
            with patch.dict(os.environ,SystemRoot=str(root)),patch.object(ctypes,'get_last_error',return_value=5,create=True),patch.object(ctypes,'WinError',side_effect=lambda value:OSError(value,'Windows process error'),create=True):
                with self.assertRaisesRegex(RuntimeError,'descendants survived'):scope.settle_helpers()
            self.assertEqual(kernel.closed,[99])

if __name__=='__main__':unittest.main()
