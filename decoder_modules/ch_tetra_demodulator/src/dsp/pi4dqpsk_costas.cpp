#include "pi4dqpsk_costas.h"

namespace dsp {
    namespace loop {
        int PI4DQPSK_COSTAS::process(int count, complex_t* in, complex_t* out) {
            for (int i = 0; i < count; i++) {
                // B3rnt optimization: fuse phase correction + PI/4 shift into
                // a single phasor call, halving sin/cos computations per sample
                ph2 += -FL_M_PI / 4.0f;
                if (ph2 >= 2 * FL_M_PI) {
                    ph2 -= 2 * FL_M_PI;
                } else if (ph2 <= -2 * FL_M_PI) {
                    ph2 += 2 * FL_M_PI;
                }
                float total_phase = -pcl.phase + ph2;
                complex_t x = in[i] * math::phasor(total_phase);
                pcl.advance(errorFunction(x));
                out[i] = x;
            }
            return count;
        }

        float PI4DQPSK_COSTAS::errorFunction(complex_t val) {
            // B3rnt: standard QPSK Costas error with amplitude weighting
            // and soft limiter for better behavior at low SNR
            float err = (math::step(val.re) * val.im) - (math::step(val.im) * val.re);
            // Amplitude weighting: reduce error contribution when signal is weak
            float a = sqrtf(val.re * val.re + val.im * val.im);
            if (a < 1.0f) {
                err *= a;
            }
            // Soft limiter: tanh-like saturation, smoother than hard clamp
            err = err / (1.0f + fabsf(err));
            return std::clamp<float>(err, -1.0f, 1.0f);
        }
    }
}
