"""One-batch macOS administrator authorisation; Python retains all edit logic."""
import errno
import os
from pathlib import Path
import sys


from .authorization import AuthorizationCancelled


def native_paths():
    macos = Path(__file__).resolve().parents[2] / 'MacOS'
    return macos / 'CabAuthorization.dylib', macos / 'CabPrivilege'


def available():
    return sys.platform == 'darwin' and all(path.is_file() for path in native_paths())


class DirectAuthorization:
    def __init__(self):
        self.library = None
        self.handle = None
        self.approved = frozenset()

    def authorize(self, paths):
        import ctypes
        self.approved = frozenset(Path(os.path.abspath(path)) for path in paths)
        library, helper = native_paths()
        self.library = ctypes.CDLL(str(library), use_errno=True)
        self.library.cab_authorize.argtypes = (ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_uint32, ctypes.POINTER(ctypes.c_int))
        self.library.cab_authorize.restype = ctypes.c_void_p
        self.library.cab_authorized_open.argtypes = (ctypes.c_void_p, ctypes.c_char_p)
        self.library.cab_authorized_open.restype = ctypes.c_int
        self.library.cab_authorization_close.argtypes = (ctypes.c_void_p,)
        self.library.cab_authorization_close.restype = None
        status = ctypes.c_int()
        encoded = [os.fsencode(path) for path in sorted(self.approved)]
        selected = (ctypes.c_char_p * len(encoded))(*encoded)
        self.handle = self.library.cab_authorize(os.fsencode(helper), selected, len(encoded), ctypes.byref(status))
        if not self.handle:
            if status.value == errno.ECANCELED:
                raise AuthorizationCancelled('Administrator authorisation cancelled; no presets edited')
            raise PermissionError('Administrator authorisation failed; no presets edited')

    def open(self, path, flags):
        path = Path(os.path.abspath(path))
        if path not in self.approved:
            raise PermissionError('File was not selected for this authorised batch')
        # Ordinary-account writes remain possible for browsed/user presets.
        try:
            return os.open(path, flags)
        except PermissionError:
            pass
        if not self.handle:
            raise PermissionError('Administrator authorisation is not active')
        fd = self.library.cab_authorized_open(self.handle, os.fsencode(path))
        if fd < 0:
            import ctypes
            raise OSError(ctypes.get_errno(), 'Administrator could not open the selected preset')
        return fd

    def close(self):
        if self.handle:
            self.library.cab_authorization_close(self.handle)
            self.handle = None
        self.approved = frozenset()
