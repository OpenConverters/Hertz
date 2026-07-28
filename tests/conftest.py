"""Backend selection for the reference test suite.

    pytest                    # the pure-Python reference implementation
    pytest --backend=cpp      # the SAME tests against the C++ engine (PyHertz)

The tests are the golden vectors shared by both implementations, so a failure
under --backend=cpp is a real disagreement between the two engines — never
something to paper over here.
"""

import importlib
import sys

_MODULES = ("comb", "detector", "filter_design", "limits", "lisn", "network", "radiated",
            "separation", "traces", "utils")


def pytest_addoption(parser):
    parser.addoption("--backend", action="store", default="python",
                     choices=("python", "cpp"),
                     help="Hertz engine under test: python (reference) or cpp (PyHertz)")


def pytest_configure(config):
    if config.getoption("--backend") != "cpp":
        return
    package = importlib.import_module("hertz_cpp")
    sys.modules["hertz"] = package
    for name in _MODULES:
        sys.modules[f"hertz.{name}"] = importlib.import_module(f"hertz_cpp.{name}")


def pytest_report_header(config):
    return f"hertz backend: {config.getoption('--backend')}"
