"""LISN models — C++ engine (PyHertz). The engine binds both a scalar and a
vector overload of the impedance getters, so a swept array is the same scalar
call per point."""

from ._engine import engine

Lisn = engine.Lisn
cispr16_lisn = engine.cispr16_lisn
cispr25_lisn = engine.cispr25_lisn
