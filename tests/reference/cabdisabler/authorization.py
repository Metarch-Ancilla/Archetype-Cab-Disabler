"""Shared permission probing and native per-platform authorisation selection."""
import os
import sys


class AuthorizationCancelled(PermissionError):
    pass


def requires_authorization(paths):
    """Probe read/write access without creating, truncating or modifying files."""
    flags = os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0) | getattr(os, 'O_BINARY', 0)
    for path in paths:
        try:
            fd = os.open(path, flags)
        except PermissionError:
            return True
        except OSError:
            continue
        else:
            os.close(fd)
    return False


def provider_factory():
    if sys.platform == 'darwin':
        from .macos_authorization import available, DirectAuthorization
    elif sys.platform == 'win32':
        from .windows_authorization import available, DirectAuthorization
    else:
        return None
    return DirectAuthorization if available() else None
