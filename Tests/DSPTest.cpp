#include "../Source/LeadDSP.h"
#include <iostream>
#include <deque>
// Vérifie : neutralité à 0 (au retard près), plafond du limiteur, symétrie stéréo, gate, sortie finie.
int main()
{
    const float ceiling = std::pow (10.f, LeadDSP::ceilingDb / 20.f) + 1e-6f;
    for (double rate : { 44100., 48000., 96000., 192000. })
    {
        { // Neutre : compression 0, gate off, gain 0 -> sortie = entrée retardée
            LeadDSP d; d.prepare (rate); std::deque<float> ref;
            for (int i = 0; i < 20000; ++i)
            {
                float s = 0.5f * std::sin (i * 0.03f), x[2] { s, s };
                ref.push_back (s); d.tick (x, 2, 0, LeadDSP::gateOffDb, 0);
                if ((int) ref.size() > d.latency()) { if (std::abs (x[0] - ref.front()) > 1e-5f) return 1; ref.pop_front(); }
            }
        }
        for (float c : { .25f, .5f, 1.f })
        { // Signal très fort + gain max : jamais au-dessus du plafond
            LeadDSP d; d.prepare (rate);
            for (int i = 0; i < 48000; ++i)
            {
                float s = float (1.5 * std::sin (i * .05) * (i % 9000 < 300 ? 1.0 : 0.2)), x[2] { s, s * 0.7f };
                d.tick (x, 2, c, -50, 12);
                if (! std::isfinite (x[0]) || std::abs (x[0]) > ceiling || std::abs (x[1]) > ceiling) return 2;
            }
        }
        { // Gate : bruit faible coupé d'au moins 20 dB
            LeadDSP d; d.prepare (rate); float last = 0;
            for (int i = 0; i < int (rate); ++i) { float s = 0.003f * std::sin (i * .2f), x[2] { s, s }; d.tick (x, 2, 0, -40, 0); last = std::max (last * .999f, std::abs (x[0])); }
            if (last > 0.0003f) return 3;
        }
    }
    std::cout << "LeadDSP : neutre a 0, plafond respecte, gate actif, sortie finie (4 frequences).\n";
}
