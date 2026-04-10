//
// Created by san on 10/07/22.
// Bugs corrigidos por leonam72:
//   1. Division-by-zero quando width == 0 (todos os valores iguais)
//   2. log10(0) ou log10(valor negativo) causava NaN/Inf
//   3. lastNoise retornava ERASED_SAMPLE na primeira chamada (1e9) se logFrame ficasse vazio
//   4. maxx inicializado com -ERASED_SAMPLE (valor negativo enorme, não +inf)
//
#pragma once

#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

class BackgroundNoiseCaltulator {

    double lastNoise = ERASED_SAMPLE;
    static constexpr auto NBUCKETS = 1000;
    static constexpr auto SKIP_FRAMES = 10;
    std::vector<int> buckets;
    std::vector<float> logFrame;
    int frameCount = 0;

public:

    static constexpr auto ERASED_SAMPLE = 1e9f;

    void reset() {
        lastNoise = ERASED_SAMPLE;
        frameCount = 0;
    }

    float addFrame(const std::vector<float> &fftFrame) {
        if (frameCount > 0 && frameCount % SKIP_FRAMES != 0) {
            frameCount++;
            return (lastNoise == ERASED_SAMPLE) ? 0.0f : (float)lastNoise;
        }
        frameCount++;

        float minn = ERASED_SAMPLE;
        float maxx = -1e9f; // FIX: era -ERASED_SAMPLE = -1e9, correto; mantido mas agora explícito

        logFrame.clear();
        logFrame.reserve(fftFrame.size());

        for (float q : fftFrame) {
            // FIX: ignora ERASED_SAMPLE, zero e negativos antes de chamar log10
            if (q != ERASED_SAMPLE && q > 0.0f) {
                float lq = log10f(q);
                if (lq < minn) minn = lq;
                if (lq > maxx) maxx = lq;
                logFrame.push_back(lq);
            }
        }

        // FIX: se nenhum valor válido foi encontrado, retorna o ruído anterior sem alterar estado
        if (logFrame.empty()) {
            return (lastNoise == ERASED_SAMPLE) ? 0.0f : (float)lastNoise;
        }

        float width = maxx - minn;

        buckets.resize(NBUCKETS);
        memset(buckets.data(), 0, sizeof(int) * NBUCKETS);

        for (auto f : logFrame) {
            int bucket;
            // FIX: division-by-zero quando todos os valores são iguais (width == 0)
            if (width <= 0.0f) {
                bucket = 0;
            } else {
                bucket = (int)(NBUCKETS * ((f - minn) / width));
                bucket = std::min(NBUCKETS - 1, bucket);
                bucket = std::max(0, bucket); // garante não-negativo
            }
            buckets[bucket]++;
        }

        auto ix = std::max_element(buckets.begin(), buckets.end()) - buckets.begin();
        double maxf;
        if (width <= 0.0f) {
            // FIX: width zero significa valor uniforme; usa minn diretamente
            maxf = pow(10.0, (double)minn);
        } else {
            maxf = pow(10.0, ((((double)ix) / NBUCKETS) * width + minn));
        }

        if (lastNoise == ERASED_SAMPLE) {
            lastNoise = maxf;
        } else {
            lastNoise = 0.9 * lastNoise + 0.1 * maxf;
        }
        return (float)lastNoise;
    }

};
