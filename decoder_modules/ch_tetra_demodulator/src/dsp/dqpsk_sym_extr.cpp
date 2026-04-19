#include "dqpsk_sym_extr.h"

namespace dsp {
    int DQPSKSymbolExtractor::process(int count, const complex_t* in, uint8_t* out) {
        for (int i = 0; i < count; i++) {
            complex_t sym_c = in[i];
            bool a = sym_c.im < 0;
            bool b = sym_c.re < 0;

            // B3rnt optimization: replace atan2-based phase distance with fast
            // cross-product error estimate. Mathematically equivalent for small
            // angles (sin(theta) ~ theta) but ~2-3x faster — no atan2 call.
            float sign_re = b ? -0.7071f : 0.7071f;
            float sign_im = a ? -0.7071f : 0.7071f;
            float raw_err = (sym_c.re * sign_im) - (sym_c.im * sign_re);
            float dist = fabsf(raw_err);

            // Amplitude gating: ignore sync metric when signal amplitude too low
            // to avoid false sync detection on noise floor
            float amp = sqrtf(sym_c.re * sym_c.re + sym_c.im * sym_c.im);
            if (amp < 0.15f) {
                dist = 0.35f; // treat as out-of-sync when signal absent
            }

            errorbuf[errorptr] = dist;
            errorptr++;
            if (errorptr >= SYNC_DETECT_BUF) {
                errorptr = 0;
            }
            errordisplayptr++;
            if (errordisplayptr >= SYNC_DETECT_DISPLAY) {
                float xerr = 0;
                for (int j = 0; j < SYNC_DETECT_BUF; j++) {
                    xerr += errorbuf[j];
                }
                xerr /= (float)SYNC_DETECT_BUF;
                standarderr = xerr;
                if (xerr >= 0.35f) {
                    sync = false;
                } else {
                    sync = true;
                }
                errordisplayptr = 0;
            }

            uint8_t sym = ((a) << 1) | (a != b);
            uint8_t phaseDiff = (sym - prev + 4) % 4;
            switch (phaseDiff) {
                case 0b00: out[i] = 0b00; break;
                case 0b01: out[i] = 0b01; break;
                case 0b10: out[i] = 0b11; break;
                case 0b11: out[i] = 0b10; break;
            }
            prev = sym;
        }
        return count;
    }
}
