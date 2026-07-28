"""Locate and import the PyHertz pybind11 module.

The build tree (cpp/build) is searched first so a working checkout needs no
install step; an installed PyHertz on sys.path is used when the build tree has
none.
"""

import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_BUILD = _REPO / "cpp" / "build"

_FOUND = list(_BUILD.glob("PyHertz*.so")) + list(_BUILD.glob("PyHertz*.pyd"))
if _FOUND and str(_BUILD) not in sys.path:
    sys.path.insert(0, str(_BUILD))

try:
    import PyHertz as engine
except ImportError as error:                                       # pragma: no cover
    # A built module the RUNNING interpreter cannot load is the common failure
    # (`python` is often an older minor than the `python3` the build targeted),
    # and "not built" would send you rebuilding what is already there.
    if _FOUND:
        _running = f"{sys.version_info.major}.{sys.version_info.minor}"
        raise ImportError(
            f"PyHertz IS built ({', '.join(sorted(p.name for p in _FOUND))}) but this "
            f"interpreter cannot load it: you are running Python {_running} "
            f"({sys.executable}). Use the interpreter it was built for (usually "
            f"`python3`), or rebuild with -DPython3_EXECUTABLE={sys.executable}."
        ) from error
    raise ImportError(
        "PyHertz (the C++ engine) is not built. Build it with:\n"
        "  cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release\n"
        "  ninja -C cpp/build PyHertz\n"
        f"(looked in {_BUILD} and on sys.path)"
    ) from error

__all__ = ["engine"]
