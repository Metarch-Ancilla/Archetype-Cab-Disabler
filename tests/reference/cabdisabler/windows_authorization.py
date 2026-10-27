"""Batch-scoped UAC file-handle broker; the Python backend stays unprivileged."""
import os
from pathlib import Path
import sys
from .authorization import AuthorizationCancelled


def native_paths():
    root = Path(sys.executable).resolve().parent.parent
    return root / 'CabAuthorization.dll', root / 'NeuralDSPCabDisabler.exe'


def available():
    return sys.platform == 'win32' and all(path.is_file() for path in native_paths())


class DirectAuthorization:
    def __init__(self):
        self.library = None
        self.handle = None
        self.approved = frozenset()

    def authorize(self, paths):
        import ctypes
        self.approved = frozenset(Path(os.path.abspath(path)) for path in paths)
        library, helper = native_paths()
        self.library = ctypes.CDLL(str(library), use_last_error=True)
        self.library.cab_windows_authorize.argtypes = (ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_wchar_p), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32))
        self.library.cab_windows_authorize.restype = ctypes.c_void_p
        self.library.cab_windows_authorized_open.argtypes = (ctypes.c_void_p, ctypes.c_wchar_p)
        self.library.cab_windows_authorized_open.restype = ctypes.c_size_t
        self.library.cab_windows_authorization_close.argtypes = (ctypes.c_void_p,)
        self.library.cab_windows_authorization_close.restype = None
        selected = sorted(map(str, self.approved))
        approved = (ctypes.c_wchar_p * len(selected))(*selected)
        status = ctypes.c_uint32()
        self.handle = self.library.cab_windows_authorize(str(helper), approved, len(selected), ctypes.byref(status))
        if not self.handle:
            if status.value == 1223:  # ERROR_CANCELLED: UAC declined.
                raise AuthorizationCancelled('Administrator authorisation cancelled; no presets edited')
            raise PermissionError(f'Administrator authorisation failed (Windows error {status.value}); no presets edited')

    def open(self, path, flags):
        path = Path(os.path.abspath(path))
        if path not in self.approved:
            raise PermissionError('File was not selected for this authorised batch')
        try:
            return os.open(path, flags)
        except PermissionError:
            pass
        if not self.handle:
            raise PermissionError('Administrator authorisation is not active')
        handle = self.library.cab_windows_authorized_open(self.handle, str(path))
        if not handle:
            import ctypes
            error = ctypes.get_last_error()
            raise OSError(error, 'Administrator could not open the selected preset')
        import msvcrt
        try:
            return msvcrt.open_osfhandle(handle, flags)
        except BaseException:
            import ctypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
            kernel.CloseHandle.restype = ctypes.c_int
            kernel.CloseHandle(handle)
            raise

    def close(self):
        if self.handle:
            self.library.cab_windows_authorization_close(self.handle)
            self.handle = None
        self.approved = frozenset()
