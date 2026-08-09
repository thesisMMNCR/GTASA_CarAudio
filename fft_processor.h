#pragma once

#include <complex>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// Must be a power of 2 for the Radix-2 FFT algorithm
#define FFT_SIZE 512

// Standard bit-reversal configuration for the FFT butterfly stages
inline void ReorderFFT(std::complex<float>* input, int n) {
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (i < j) std::swap(input[i], input[j]);
        int bit = n >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
    }
}

// Complex Radix-2 In-Place Fast Fourier Transform
inline void ComputeFFT(std::complex<float>* samples, int n) {
    ReorderFFT(samples, n);
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * M_PI / len;
        std::complex<float> wlen(cosf(angle), sinf(angle));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; j++) {
                std::complex<float> u = samples[i + j];
                std::complex<float> v = samples[i + j + len / 2] * w;
                samples[i + j]           = u + v;
                samples[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

