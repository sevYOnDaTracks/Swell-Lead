// Outil hors ligne : capture de l'interface et rendu audio avec le vrai moteur de Swell Lead.
//   LeadTool snapshot <sortie.png> [voix.wav] [comp gate gain]
//   LeadTool render <entree.wav> <sortie.wav> <comp> <gate> <gain>
#include "../Source/Plugin.cpp"
#include <juce_audio_formats/juce_audio_formats.h>

static bool readWav (const juce::File& f, juce::AudioBuffer<float>& b, double& rate)
{
    juce::AudioFormatManager m; m.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (m.createReaderFor (f));
    if (! r) return false;
    b.setSize ((int) juce::jmin (2u, r->numChannels), (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    rate = r->sampleRate;
    return true;
}

static void setParams (LeadProcessor& p, float comp, float gate, float gain)
{
    auto set = [&p] (const char* id, float v) { auto* q = p.state.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
    set ("comp", comp); set ("gate", gate); set ("gain", gain);
}

// Passe le tampon dans le processeur, en compensant la latence. Retourne le résultat.
static juce::AudioBuffer<float> process (LeadProcessor& p, const juce::AudioBuffer<float>& in, double rate, int stopAt = -1)
{
    const int block = 512, ch = in.getNumChannels();
    p.setPlayConfigDetails (ch, ch, rate, block);
    p.prepareToPlay (rate, block);
    const int lat = p.getLatencySamples(), n = stopAt > 0 ? stopAt : in.getNumSamples();
    juce::AudioBuffer<float> out (ch, n), tmp (ch, block);
    for (int pos = 0; pos < n + lat; pos += block)
    {
        const int len = juce::jmin (block, n + lat - pos);
        tmp.clear();
        for (int c = 0; c < ch; ++c) if (pos < n) tmp.copyFrom (c, 0, in, c, pos, juce::jmin (len, n - pos));
        juce::AudioBuffer<float> view (tmp.getArrayOfWritePointers(), ch, len);
        p.render (view, false);
        for (int i = 0; i < len; ++i)
        {
            const int dst = pos + i - lat;
            if (dst >= 0 && dst < n) for (int c = 0; c < ch; ++c) out.setSample (c, dst, view.getSample (c, i));
        }
    }
    return out;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    swell::Licence::unlockForTesting();
    juce::StringArray a; for (int i = 1; i < argc; ++i) a.add (juce::String::fromUTF8 (argv[i]));
    const auto cwd = juce::File::getCurrentWorkingDirectory();

    if (a[0] == "render" && a.size() >= 6)
    {
        juce::AudioBuffer<float> in; double rate = 0;
        if (! readWav (cwd.getChildFile (a[1]), in, rate)) { std::cerr << "lecture impossible\n"; return 1; }
        LeadProcessor p; setParams (p, a[3].getFloatValue(), a[4].getFloatValue(), a[5].getFloatValue());
        auto out = process (p, in, rate);
        auto file = cwd.getChildFile (a[2]); file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (file), rate, (unsigned) out.getNumChannels(), 24, {}, 0));
        w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
        std::cout << "rendu : " << file.getFullPathName() << "\n";
        return 0;
    }
    if (a[0] == "snapshot" && a.size() >= 2)
    {
        LeadProcessor p;
        setParams (p, a.size() > 3 ? a[3].getFloatValue() : 45.f, a.size() > 4 ? a[4].getFloatValue() : -50.f, a.size() > 5 ? a[5].getFloatValue() : 2.f);
        if (a.size() > 2)
        {   // Fait passer la voix jusqu'à un passage fort pour que les vumètres bougent.
            juce::AudioBuffer<float> in; double rate = 0;
            if (readWav (cwd.getChildFile (a[2]), in, rate)) process (p, in, rate, juce::jmin (in.getNumSamples(), int (rate * 3.2)));
        }
        LeadEditor ed (p);
        ed.setSize (680, 500);
        auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 2.f);
        auto file = cwd.getChildFile (a[1]); file.deleteFile();
        juce::FileOutputStream os (file); juce::PNGImageFormat().writeImageToStream (img, os);
        std::cout << "capture : " << file.getFullPathName() << "\n";
        return 0;
    }
    std::cerr << "usage : LeadTool snapshot <png> [voix.wav] [comp gate gain] | render <in> <out> <comp> <gate> <gain>\n";
    return 2;
}
