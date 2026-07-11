import os

from .tar import TarHandler
from .zip import ZipHandler
from .lha import LhaHandler
from .rar import RarHandler
from .sevenz import SevenZipHandler

# Registry of available handlers
HANDLERS = [
    TarHandler,
    ZipHandler,
    LhaHandler,
    RarHandler,
    SevenZipHandler,
]

def get_handler(path):
    """Returns an instantiated handler if the archive format is supported, else None."""
    if not os.path.isfile(path):
        return None
    for h_class in HANDLERS:
        try:
            if h_class.can_handle(path):
                return h_class(path)
        except Exception:
            pass
    return None
