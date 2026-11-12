#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Moteur de Swell Lead : Gate -> Compression (remontée de volume auto) -> Gain -> Limiteur.
// Détection liée en stéréo (l'image ne bouge pas). Aucune allocation dans le thread audio.
class LeadDSP
{
public:
    static constexpr float ceilingDb = -0.5f;   // plafond du limiteur
    static constexpr float gateOffDb = -80.f;   // Gate tout à gauche = éteint

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        lookahead = std::max (1, (int) std::lround (0.0015 * fs));
        for (auto& d : delay) d.assign ((size_t) lookahead + 1, 0.f);
        required.assign ((size_t) lookahead + 1, 1.f);
        gateDetAtt = coeff (0.3); gateDetRel = coeff (30);
        gateOpenCoef = coeff (1.0); gateCloseCoef = coeff (90);
        compAtt = coeff (4); compRel = coeff (140);
        limAtt = (float) (1.0 - std::exp (-3.0 / lookahead));
        limRel = 1.f - coeff (120);
        holdSamples = (int) (0.06 * fs);
        reset();
    }

    void reset()
    {
        for (auto& d : delay) std::fill (d.begin(), d.end(), 0.f);
        std::fill (required.begin(), required.end(), 1.f);
        pos = 0; gateEnv = 0; gateGain = 1; gateHold = 0; gateOpen = false;
        grDb = 0; limGain = 1;
        meterGr = 0; meterLim = 1; meterGateOpen = false;
    }

    int latency() const noexcept { return lookahead; }

    // comp : 0..1   gateDb : -80 (off) .. -20   outDb : -12..+12
    void tick (float* x, int channels, float comp, float gateDb, float outDb)
    {
        channels = std::min (channels, 2);
        float peak = 0;
        for (int c = 0; c < channels; ++c) peak = std::max (peak, std::abs (x[c]));

        // --- Gate : coupe le bruit entre les phrases (atténuation -40 dB, ouverture rapide, fermeture douce)
        float g = 1.f;
        if (gateDb > gateOffDb + 0.5f)
        {
            const float a = peak > gateEnv ? gateDetAtt : gateDetRel;
            gateEnv = a * gateEnv + (1 - a) * peak;
            const float envDb = toDb (gateEnv);
            if (envDb > gateDb) { gateOpen = true; gateHold = holdSamples; }
            else if (envDb < gateDb - 6.f) { if (gateHold > 0) --gateHold; else gateOpen = false; }
            const float target = gateOpen ? 1.f : 0.01f;
            const float k = target > gateGain ? gateOpenCoef : gateCloseCoef;
            gateGain = k * gateGain + (1 - k) * target;
            g = gateGain;
        }
        else { gateOpen = true; gateGain = 1; }
        meterGateOpen = gateOpen;

        // --- Compression : un bouton pilote seuil, ratio et remontée de volume
        const float c = std::clamp (comp, 0.f, 1.f);
        const float threshold = -8.f - 24.f * c;
        const float ratio = 1.f + 5.f * c;
        const float slope = 1.f - 1.f / ratio;
        const float knee = 6.f;
        const float level = toDb (peak * g);
        const float over = level - threshold;
        float target = 0;
        if (over > knee * 0.5f) target = -slope * over;
        else if (over > -knee * 0.5f) { const float t = over + knee * 0.5f; target = -slope * t * t / (2 * knee); }
        const float k = target < grDb ? compAtt : compRel;
        grDb = k * grDb + (1 - k) * target;
        const float makeup = (0.55f + 0.25f * c) * -threshold * slope; // plus on compresse, plus la voix ressort
        meterGr = std::min (meterGr, grDb);

        const float gain = g * fromDb (grDb + makeup + outDb);

        // --- Limiteur avec anticipation : jamais au-dessus du plafond
        const float ceiling = fromDb (ceilingDb);
        float outPeak = 0;
        float y[2] {};
        for (int c2 = 0; c2 < channels; ++c2) { y[c2] = x[c2] * gain; outPeak = std::max (outPeak, std::abs (y[c2])); }
        required[(size_t) pos] = outPeak > ceiling ? ceiling / outPeak : 1.f;
        float need = 1.f;
        for (float r : required) need = std::min (need, r);
        limGain += (need - limGain) * (need < limGain ? limAtt : limRel);
        limGain = std::min (limGain, 1.f);
        meterLim = std::min (meterLim, limGain);

        const int readPos = (pos + 1) % (lookahead + 1);
        for (int c2 = 0; c2 < channels; ++c2)
        {
            auto& d = delay[(size_t) c2];
            d[(size_t) pos] = y[c2];
            x[c2] = std::clamp (d[(size_t) readPos] * limGain, -ceiling, ceiling);
        }
        pos = readPos;
    }

    // Mesures lues (et remises à zéro) par le processeur une fois par bloc.
    float takeCompReductionDb() noexcept { const float v = meterGr; meterGr = 0; return v; }
    float takeLimiterGain() noexcept { const float v = meterLim; meterLim = 1; return v; }
    bool gateIsOpen() const noexcept { return meterGateOpen; }

private:
    float coeff (double ms) const { return (float) std::exp (-1.0 / (ms * 0.001 * fs)); }
    static float toDb (float v) { return 20.f * std::log10 (std::max (v, 1.0e-6f)); }
    static float fromDb (float d) { return std::pow (10.f, d / 20.f); }

    double fs = 48000;
    int lookahead = 72, pos = 0, holdSamples = 2880, gateHold = 0;
    std::vector<float> delay[2], required;
    float gateDetAtt = 0, gateDetRel = 0, gateOpenCoef = 0, gateCloseCoef = 0;
    float compAtt = 0, compRel = 0, limAtt = 0, limRel = 0;
    float gateEnv = 0, gateGain = 1, grDb = 0, limGain = 1;
    bool gateOpen = false, meterGateOpen = false;
    float meterGr = 0, meterLim = 1;
};
