#pragma once
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "hertz/MkfFft.hpp"
#include "hertz/Utils.hpp"

namespace Hertz {

// CISPR 16-1-1 measuring-receiver emulation on sampled time-domain data, per
// Krug & Russer, "Quasi-peak detector model for a time-domain measurement
// system", IEEE Trans. EMC 47(2), 2005: short-time FFT with a Gaussian window
// whose 6 dB bandwidth equals the CISPR RBW, per-bin envelope, then the
// quasi-peak charge/discharge detector and the critically damped meter filter
// as discrete-time one-pole stages.
//
// Readings are calibrated so an unmodulated sine reads its RMS value in dBµV
// (the CISPR receiver convention): for a CW tone, peak, quasi-peak and average
// coincide.
//
// Frames are processed streaming — one FFT frame in flight — so memory stays
// bounded for arbitrarily long captures (the property that keeps long scope
// records viable in wasm32).

struct CisprBand {
    const char* name;
    double fMinHz;
    double fMaxHz;
    double rbw6dbHz;
    double tauChargeS;
    double tauDischargeS;
    double tauMeterS;
};

inline constexpr CisprBand BAND_A{"A", 9e3, 150e3, 200.0, 45e-3, 500e-3, 160e-3};
inline constexpr CisprBand BAND_B{"B", 150e3, 30e6, 9e3, 1e-3, 160e-3, 160e-3};
inline constexpr CisprBand BAND_C{"C", 30e6, 300e6, 120e3, 1e-3, 550e-3, 100e-3};
inline constexpr CisprBand BAND_D{"D", 300e6, 1e9, 120e3, 1e-3, 550e-3, 100e-3};

inline const CisprBand& band_for_frequency(double fHz) {
    for (const CisprBand* band : {&BAND_A, &BAND_B, &BAND_C, &BAND_D}) {
        if (band->fMinHz <= fHz && fHz <= band->fMaxHz) {
            return *band;
        }
    }
    throw std::invalid_argument(std::to_string(fHz) + " Hz is outside CISPR bands A-D");
}

namespace detail {

struct StftPlan {
    std::vector<double> window;
    double windowSum;
    size_t winLen;
    size_t hop;
    size_t nfft;
    size_t bins;

    StftPlan(double fsHz, const CisprBand& band, double overlap, size_t signalLength) {
        if (fsHz <= 2.0 * band.fMinHz) {
            throw std::invalid_argument(std::string("sample rate cannot represent band ") + band.name);
        }
        if (!(0.0 <= overlap && overlap < 1.0)) {
            throw std::invalid_argument("overlap must be in [0, 1)");
        }
        double sigmaF = band.rbw6dbHz / (2.0 * std::sqrt(2.0 * std::numbers::ln2));
        double sigmaT = 1.0 / (2.0 * std::numbers::pi * sigmaF);
        size_t half = static_cast<size_t>(std::ceil(4.0 * sigmaT * fsHz));
        winLen = 2 * half + 1;
        if (winLen > signalLength) {
            throw std::invalid_argument("signal too short: " + std::to_string(signalLength) +
                                        " samples < one " + std::to_string(winLen) +
                                        "-sample analysis window");
        }
        window.resize(winLen);
        windowSum = 0.0;
        for (size_t i = 0; i < winLen; ++i) {
            double n = static_cast<double>(i) - static_cast<double>(half);
            window[i] = std::exp(-0.5 * std::pow(n / (sigmaT * fsHz), 2));
            windowSum += window[i];
        }
        hop = std::max<size_t>(1, static_cast<size_t>(std::llround(winLen * (1.0 - overlap))));
        nfft = 1;
        while (nfft < winLen) {
            nfft <<= 1;
        }
        bins = nfft / 2 + 1;
    }
};

// One-pole quasi-peak stage followed by the two-pole critically damped meter.
class QuasiPeakChain {
  public:
    QuasiPeakChain(size_t bins, double dtS, const CisprBand& band)
        : _chargeAlpha(dtS / band.tauChargeS),
          _dischargeKeep(1.0 - dtS / band.tauDischargeS),
          _meterAlpha(dtS / band.tauMeterS),
          _detector(bins, 0.0),
          _m1(bins, 0.0),
          _m2(bins, 0.0),
          _reading(bins, 0.0) {
        if (dtS >= band.tauChargeS / 5.0) {
            throw std::invalid_argument(
                "envelope sample interval too coarse for the quasi-peak charge time "
                "constant — raise overlap or the sample rate");
        }
    }

    void push(const std::vector<double>& envelopeRow) {
        for (size_t k = 0; k < _detector.size(); ++k) {
            double e = envelopeRow[k];
            double v = _detector[k];
            _detector[k] = e > v ? v + (e - v) * _chargeAlpha : v * _dischargeKeep;
            _m1[k] += (_detector[k] - _m1[k]) * _meterAlpha;
            _m2[k] += (_m1[k] - _m2[k]) * _meterAlpha;
            _reading[k] = std::max(_reading[k], _m2[k]);
        }
    }

    const std::vector<double>& reading() const { return _reading; }

  private:
    double _chargeAlpha;
    double _dischargeKeep;
    double _meterAlpha;
    std::vector<double> _detector;
    std::vector<double> _m1;
    std::vector<double> _m2;
    std::vector<double> _reading;
};

class AverageChain {
  public:
    AverageChain(size_t bins, double dtS, const CisprBand& band)
        : _meterAlpha(dtS / band.tauMeterS), _m1(bins, 0.0), _m2(bins, 0.0), _reading(bins, 0.0) {}

    void push(const std::vector<double>& envelopeRow) {
        for (size_t k = 0; k < _m1.size(); ++k) {
            _m1[k] += (envelopeRow[k] - _m1[k]) * _meterAlpha;
            _m2[k] += (_m1[k] - _m2[k]) * _meterAlpha;
            _reading[k] = std::max(_reading[k], _m2[k]);
        }
    }

    const std::vector<double>& reading() const { return _reading; }

  private:
    double _meterAlpha;
    std::vector<double> _m1;
    std::vector<double> _m2;
    std::vector<double> _reading;
};

}  // namespace detail

struct ReceiverReading {
    std::vector<double> frequenciesHz;
    std::vector<double> peakDbuv;
    std::vector<double> quasiPeakDbuv;
    std::vector<double> averageDbuv;
};

// Per-bin amplitude envelope of x (frames x bins); a tone at bin center of
// amplitude A yields A. Materializes the whole matrix — test/analysis sizes
// only; measure() below streams instead.
inline std::vector<std::vector<double>> stft_envelope(const std::vector<double>& x, double fsHz,
                                                      const CisprBand& band,
                                                      std::vector<double>* frequenciesHz = nullptr,
                                                      double overlap = 0.9) {
    detail::StftPlan plan(fsHz, band, overlap, x.size());
    if (frequenciesHz != nullptr) {
        frequenciesHz->resize(plan.bins);
        for (size_t k = 0; k < plan.bins; ++k) {
            (*frequenciesHz)[k] = static_cast<double>(k) * fsHz / static_cast<double>(plan.nfft);
        }
    }
    std::vector<std::vector<double>> envelope;
    std::vector<std::complex<double>> frame(plan.nfft);
    for (size_t start = 0; start + plan.winLen <= x.size(); start += plan.hop) {
        std::fill(frame.begin(), frame.end(), std::complex<double>(0.0, 0.0));
        for (size_t i = 0; i < plan.winLen; ++i) {
            frame[i] = x[start + i] * plan.window[i];
        }
        OpenMagnetics::fft(frame);
        std::vector<double> row(plan.bins);
        for (size_t k = 0; k < plan.bins; ++k) {
            row[k] = 2.0 * std::abs(frame[k]) / plan.windowSum;
        }
        envelope.push_back(std::move(row));
    }
    return envelope;
}

// Dwell until the 2nd-order meter is settled well under 0.1 dB. A record
// shorter than a few meter time constants would report the meter mid-rise —
// silently 10-20 dB low on a typical scope capture (a false-PASS generator).
// The record is treated as one period of a stationary signal and the weighting
// chains cycle over its envelope until settled, as a receiver dwells on it.
inline constexpr double SETTLE_METER_TAUS = 7.0;

// EMI-receiver readings of a sampled signal: peak, quasi-peak and average per
// FFT bin, RMS-of-CW calibrated dBµV.
inline ReceiverReading measure(const std::vector<double>& x, double fsHz, const CisprBand& band,
                               double overlap = 0.9) {
    detail::StftPlan plan(fsHz, band, overlap, x.size());
    size_t frames = (x.size() - plan.winLen) / plan.hop + 1;
    if (frames < 2) {
        throw std::invalid_argument("signal yields fewer than two analysis frames");
    }
    double dt = static_cast<double>(plan.hop) / fsHz;
    double needed = SETTLE_METER_TAUS * std::max(band.tauMeterS, band.tauDischargeS / 2.0);
    size_t repetitions = std::max<size_t>(
        1, static_cast<size_t>(std::ceil(needed / (static_cast<double>(frames) * dt))));

    detail::QuasiPeakChain quasiPeak(plan.bins, dt, band);
    detail::AverageChain average(plan.bins, dt, band);
    std::vector<double> peak(plan.bins, 0.0);

    // float32 envelope buffer so short records can be cycled; ~4 B/bin/frame.
    std::vector<float> envelope(frames * plan.bins);
    std::vector<std::complex<double>> frame(plan.nfft);
    std::vector<double> row(plan.bins);
    size_t frameIndex = 0;
    for (size_t start = 0; start + plan.winLen <= x.size(); start += plan.hop, ++frameIndex) {
        std::fill(frame.begin(), frame.end(), std::complex<double>(0.0, 0.0));
        for (size_t i = 0; i < plan.winLen; ++i) {
            frame[i] = x[start + i] * plan.window[i];
        }
        OpenMagnetics::fft(frame);
        float* out = &envelope[frameIndex * plan.bins];
        for (size_t k = 0; k < plan.bins; ++k) {
            double value = 2.0 * std::abs(frame[k]) / plan.windowSum;
            peak[k] = std::max(peak[k], value);
            out[k] = static_cast<float>(value);
        }
    }
    for (size_t rep = 0; rep < repetitions; ++rep) {
        for (size_t f = 0; f < frames; ++f) {
            const float* source = &envelope[f * plan.bins];
            for (size_t k = 0; k < plan.bins; ++k) {
                row[k] = source[k];
            }
            quasiPeak.push(row);
            average.push(row);
        }
    }

    ReceiverReading reading;
    reading.frequenciesHz.resize(plan.bins);
    reading.peakDbuv.resize(plan.bins);
    reading.quasiPeakDbuv.resize(plan.bins);
    reading.averageDbuv.resize(plan.bins);
    const double toRms = 1.0 / std::numbers::sqrt2;
    for (size_t k = 0; k < plan.bins; ++k) {
        reading.frequenciesHz[k] = static_cast<double>(k) * fsHz / static_cast<double>(plan.nfft);
        reading.peakDbuv[k] = dbuv_from_vrms(peak[k] * toRms);
        reading.quasiPeakDbuv[k] = dbuv_from_vrms(quasiPeak.reading()[k] * toRms);
        reading.averageDbuv[k] = dbuv_from_vrms(average.reading()[k] * toRms);
    }
    return reading;
}

}  // namespace Hertz
