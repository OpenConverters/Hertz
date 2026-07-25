#pragma once
#include <complex>
#include <vector>

// MKF's radix-2 Cooley-Tukey FFT (defined in MKF src/processors/Inputs.cpp with
// external linkage but no public header). Declared here and resolved by linking
// libMKF — native and WASM builds alike. Input size must be a power of two.
namespace OpenMagnetics {
void fft(std::vector<std::complex<double>>& x);
void ifft(std::vector<std::complex<double>>& x);
}  // namespace OpenMagnetics
