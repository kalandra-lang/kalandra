"""Own this disposable editor test's descendants without global process killing."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path


class EditorProcessScope:
    """A nested Windows job: only owned system WSL probe helpers may be reaped.

    No kill-on-close flag: closing this handle must not terminate Python itself
    or hide an orphaned language server. Unexpected surviving children fail.
    """
    def __init__(self):
        self.kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        signatures = {
            'CreateJobObjectW': ([ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            'GetCurrentProcess': ([], wintypes.HANDLE),
            'AssignProcessToJobObject': ([wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            'QueryInformationJobObject': ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
            'OpenProcess': ([wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            'QueryFullProcessImageNameW': ([wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
            'TerminateProcess': ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            'WaitForSingleObject': ([wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
            'CloseHandle': ([wintypes.HANDLE], wintypes.BOOL),
        }
        for name, (arguments, result) in signatures.items():
            function = getattr(self.kernel, name)
            function.argtypes, function.restype = arguments, result
        self.job = self.kernel.CreateJobObjectW(None, None)
        if not self.job:
            raise ctypes.WinError(ctypes.get_last_error())
        if not self.kernel.AssignProcessToJobObject(self.job, self.kernel.GetCurrentProcess()):
            error = ctypes.WinError(ctypes.get_last_error())
            self.close()
            raise error

    def close(self):
        if self.job:
            self.kernel.CloseHandle(self.job)
            self.job = None

    def children(self):
        # JOBOBJECT_BASIC_PROCESS_ID_LIST has two DWORD counts followed by
        # pointer-sized process IDs on both native Windows architectures.
        for capacity in (64, 256, 1024, 4096):
            buffer = ctypes.create_string_buffer(8 + capacity * ctypes.sizeof(ctypes.c_size_t))
            if self.kernel.QueryInformationJobObject(self.job, 3, buffer, len(buffer), None):
                count = wintypes.DWORD.from_buffer(buffer, 4).value
                if count > capacity:
                    raise RuntimeError('Invalid editor job process count')
                ids = (ctypes.c_size_t * count).from_buffer(buffer, 8)
                return [pid for pid in ids if pid != os.getpid()]
            error = ctypes.get_last_error()
            if error != 234:  # ERROR_MORE_DATA
                raise ctypes.WinError(error)
        raise RuntimeError('Editor job process limit exceeded')

    def settle_helpers(self):
        system = (Path(os.environ['SystemRoot']) / 'System32').resolve()
        handles = []
        try:
            # Keep opened process handles through validation and termination;
            # a recycled PID cannot redirect cleanup to an unrelated process.
            for pid in self.children():
                handle = self.kernel.OpenProcess(0x1000 | 0x100000 | 1, False, pid)
                if not handle:
                    if ctypes.get_last_error() == 87:  # exited before OpenProcess
                        continue
                    raise ctypes.WinError(ctypes.get_last_error())
                handles.append(handle)
                if self.kernel.WaitForSingleObject(handle, 0) == 0:
                    continue
                image = ctypes.create_unicode_buffer(32768)
                length = wintypes.DWORD(len(image))
                if not self.kernel.QueryFullProcessImageNameW(handle, 0, image, ctypes.byref(length)):
                    raise ctypes.WinError(ctypes.get_last_error())
                executable = Path(image.value).resolve()
                if executable.parent != system or executable.name.lower() not in ('wsl.exe', 'conhost.exe'):
                    raise RuntimeError(f'Unexpected surviving editor child {pid}: {executable}')
            for handle in handles:
                if self.kernel.WaitForSingleObject(handle, 0) == 0:
                    continue
                print('Reaping owned VS Code system WSL probe helper', flush=True)
                if not self.kernel.TerminateProcess(handle, 0):
                    error = ctypes.get_last_error()
                    if self.kernel.WaitForSingleObject(handle, 0) != 0:
                        raise ctypes.WinError(error)
                if self.kernel.WaitForSingleObject(handle, 5000) != 0:
                    raise RuntimeError('Owned editor helper did not settle')
            if self.children():
                raise RuntimeError('Editor descendants survived helper settlement')
        finally:
            for handle in handles:
                self.kernel.CloseHandle(handle)
