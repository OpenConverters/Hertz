"""CM/DM separation — C++ engine (PyHertz)."""

import numpy as np

from ._engine import engine


def separate(v_line, v_neutral):
    """(common_mode, differential_mode) from the two line quantities.

    Real records and complex spectra both go to the engine as-is: taking the
    real part of a complex spectrum would throw away the relative phase the
    separation is built on.
    """
    a = np.asarray(v_line)
    b = np.asarray(v_neutral)
    complex_input = np.iscomplexobj(a) or np.iscomplexobj(b)
    dtype = complex if complex_input else float
    a, b = a.astype(dtype, copy=False), b.astype(dtype, copy=False)
    # length and emptiness are the engine's checks; only the ndim>1 shape
    # bookkeeping (which the flat engine call cannot see) is done here
    cm, dm = engine.separate(a.ravel().tolist(), b.ravel().tolist())
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} vs {b.shape}")
    return (np.asarray(cm, dtype=dtype).reshape(a.shape),
            np.asarray(dm, dtype=dtype).reshape(a.shape))
