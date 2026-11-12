#include "ResizableEditor.h"
#include "Licence/SwellLicence.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include "BinaryData.h"
#include "LeadDSP.h"

static juce::String fr (const char* utf8) { return juce::String::fromUTF8 (utf8); }

namespace colours
{
const juce::Colour bg { 0xff1b1c20 }, face { 0xff26272c }, faceEdge { 0xff45464b }, track { 0xff34353b },
    ink { 0xffeeeef1 }, muted { 0xff9a9ba3 }, accent { 0xffff4d8d }, accentSoft { 0xffffb3cc }, open { 0xff5dd39e };
}

// ---------------------------------------------------------------------------------------------------------------
class LeadProcessor : public juce::AudioProcessor
{
public:
    // Licence Swell : partagée par toutes les instances ; silence tant que le plugin n'est pas activé.
    juce::SharedResourcePointer<swell::Licence> licence;
    juce::AudioProcessorValueTreeState state;

    // Mesures pour l'interface (écrites par le thread audio).
    std::atomic<float> inLevel { 0 }, outLevel { 0 }, compGrDb { 0 }, limiterGain { 1 };
    std::atomic<bool> gateOpen { true };

    LeadProcessor()
        : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          state (*this, nullptr, "Lead", layout())
    {
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout layout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "comp", 1 }, "Compression",
                                                            juce::NormalisableRange<float> (0, 100, .1f), 40,
                                                            juce::AudioParameterFloatAttributes().withLabel ("%")));
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gate", 1 }, "Gate",
                                                            juce::NormalisableRange<float> (LeadDSP::gateOffDb, -20, .1f), LeadDSP::gateOffDb,
                                                            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gain", 1 }, "Gain",
                                                            juce::NormalisableRange<float> (-12, 12, .1f), 0,
                                                            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "bypass", 1 }, "Bypass", false));
        return l;
    }

    const juce::String getName() const override { return "Swell Lead"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }

    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        const auto out = l.getMainOutputChannelSet();
        return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) && out == l.getMainInputChannelSet();
    }

    void prepareToPlay (double sr, int block) override
    {
        dsp.prepare (sr);
        setLatencySamples (dsp.latency());
        const auto channels = (juce::uint32) juce::jmax (1, getTotalNumInputChannels());
        mixer.prepare ({ sr, (juce::uint32) block, channels });
        mixer.setWetLatency ((float) dsp.latency());
        mixer.setMixingRule (juce::dsp::DryWetMixingRule::linear);
        mixer.reset();
        comp.reset (sr, .05); comp.setCurrentAndTargetValue (param ("comp") / 100);
        gate.reset (sr, .05); gate.setCurrentAndTargetValue (param ("gate"));
        gain.reset (sr, .05); gain.setCurrentAndTargetValue (param ("gain"));
    }
    void releaseResources() override {}

    void render (juce::AudioBuffer<float>& b, bool hostBypass)
    {
        juce::ScopedNoDenormals noDenormals;
        const int n = b.getNumSamples(), channels = juce::jmin (2, b.getNumChannels());
        if (n == 0 || channels == 0) return;

        float inPeak = 0;
        for (int c = 0; c < channels; ++c) inPeak = juce::jmax (inPeak, b.getMagnitude (c, 0, n));

        juce::dsp::AudioBlock<float> block (b);
        mixer.pushDrySamples (block);
        mixer.setWetMixProportion (hostBypass || param ("bypass") > .5f ? 0.f : 1.f);
        comp.setTargetValue (param ("comp") / 100);
        gate.setTargetValue (param ("gate"));
        gain.setTargetValue (param ("gain"));

        for (int i = 0; i < n; ++i)
        {
            float frame[2] {};
            for (int c = 0; c < channels; ++c) frame[c] = b.getSample (c, i);
            dsp.tick (frame, channels, comp.getNextValue(), gate.getNextValue(), gain.getNextValue());
            for (int c = 0; c < channels; ++c) b.setSample (c, i, frame[c]);
        }
        mixer.mixWetSamples (block);

        float outPeak = 0;
        for (int c = 0; c < channels; ++c) outPeak = juce::jmax (outPeak, b.getMagnitude (c, 0, n));
        inLevel = juce::jmax (inPeak, inLevel.load() * .8f);
        outLevel = juce::jmax (outPeak, outLevel.load() * .8f);
        compGrDb = juce::jmin (dsp.takeCompReductionDb(), compGrDb.load() * .85f);
        limiterGain = juce::jmin (dsp.takeLimiterGain(), 1.f - (1.f - limiterGain.load()) * .85f);
        gateOpen = dsp.gateIsOpen();
    }

    juce::AudioProcessorParameter* getBypassParameter() const override { return state.getParameter ("bypass"); }
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        if (! licence->isUnlocked()) { b.clear(); return; }
        render (b, false);
    }
    void processBlockBypassed (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override { render (b, true); }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& out) override
    {
        if (auto xml = state.copyState().createXml()) copyXmlToBinary (*xml, out);
    }
    void setStateInformation (const void* data, int size) override
    {
        auto xml = getXmlFromBinary (data, size);
        if (xml && xml->hasTagName (state.state.getType())) state.replaceState (juce::ValueTree::fromXml (*xml));
    }

    float param (const char* id) const { return state.getRawParameterValue (id)->load(); }

private:
    LeadDSP dsp;
    juce::dsp::DryWetMixer<float> mixer { 512 };
    juce::SmoothedValue<float> comp, gate, gain;
};

// ---------------------------------------------------------------------------------------------------------------
class LeadLook : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s) override
    {
        const bool big = s.getWidth() > 200;
        const float stroke = big ? 8.f : 6.f;
        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (stroke);
        const auto centre = r.getCentre();
        const float radius = r.getWidth() / 2;

        juce::Path track, arc;
        track.addCentredArc (centre.x, centre.y, radius, radius, 0, start, end, true);
        // Le Gain part du centre (0 dB), les autres de la gauche.
        const bool bipolar = s.getName() == "Gain";
        const float from = bipolar ? (start + end) / 2 : start;
        const float to = start + pos * (end - start);
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0, juce::jmin (from, to), juce::jmax (from, to), true);
        g.setColour (colours::track);
        g.strokePath (track, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colours::accent);
        if (std::abs (to - from) > 0.01f)
            g.strokePath (arc, juce::PathStrokeType (stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        auto faceArea = juce::Rectangle<float> (radius * 1.55f, radius * 1.55f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2e2f35), faceArea.getTopLeft(), juce::Colour (0xff1f2024), faceArea.getBottomRight(), false));
        g.fillEllipse (faceArea);
        g.setColour (colours::faceEdge);
        g.drawEllipse (faceArea, 1);

        const float fr = faceArea.getWidth() / 2;
        auto p = centre.getPointOnCircumference (fr * 0.93f, to), q = centre.getPointOnCircumference (fr * 0.70f, to);
        g.setColour (colours::accent);
        g.drawLine ({ p, q }, big ? 5.f : 3.5f);
    }
};

class LeadEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit LeadEditor (LeadProcessor& p) : AudioProcessorEditor (p), proc (p)
    {
        setSize (680, 500);
        logo = juce::ImageCache::getFromMemory (BinaryData::swellmark_png, BinaryData::swellmark_pngSize);
        setupKnob (compKnob, "Compression", 40);
        setupKnob (gateKnob, "Gate", LeadDSP::gateOffDb);
        setupKnob (gainKnob, "Gain", 0);
        bypass.setClickingTogglesState (true);
        bypass.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff25262b));
        bypass.setColour (juce::TextButton::buttonOnColourId, colours::accent);
        bypass.setColour (juce::ComboBox::outlineColourId, colours::faceEdge);
        addAndMakeVisible (bypass);
        compAttach = std::make_unique<SliderAttachment> (proc.state, "comp", compKnob);
        gateAttach = std::make_unique<SliderAttachment> (proc.state, "gate", gateKnob);
        gainAttach = std::make_unique<SliderAttachment> (proc.state, "gain", gainKnob);
        bypassAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.state, "bypass", bypass);
        startTimerHz (30);
    }
    ~LeadEditor() override
    {
        for (auto* k : { &compKnob, &gateKnob, &gainKnob }) k->setLookAndFeel (nullptr);
    }

    void resized() override
    {
        compKnob.setBounds (210, 122, 260, 260);
        gateKnob.setBounds (55, 197, 130, 130);
        gainKnob.setBounds (495, 197, 130, 130);
        bypass.setBounds (566, 33, 86, 34);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (colours::bg);
        // En-tête
        g.drawImageWithin (logo, 30, 30, 36, 33, juce::RectanglePlacement::centred);
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (25.f, juce::Font::bold));
        g.drawText ("swell", 76, 24, 120, 30, juce::Justification::left);
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (10.f));
        g.drawText ("A U D I O", 77, 52, 120, 14, juce::Justification::left);
        g.setColour (colours::track);
        g.drawVerticalLine (160, 30, 68);
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (30.f, juce::Font::bold));
        g.drawText ("L E A D", 176, 30, 200, 36, juce::Justification::left);
        g.setColour (colours::track);
        g.drawHorizontalLine (92, 0, (float) getWidth());
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (13.f));
        g.drawText (fr ("Ta voix devant. Sans prise de tête."), 0, 100, getWidth(), 20, juce::Justification::centred);

        // Titres et légendes
        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (15.f, juce::Font::bold));
        g.drawText ("GATE", 55, 168, 130, 20, juce::Justification::centred);
        g.drawText ("GAIN", 495, 168, 130, 20, juce::Justification::centred);
        g.setFont (juce::FontOptions (19.f, juce::Font::bold));
        g.drawText ("C O M P R E S S I O N", 0, 384, getWidth(), 24, juce::Justification::centred);
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (12.f));
        g.drawText ("Coupe le bruit", 55, 330, 130, 18, juce::Justification::centred);
        g.drawText (fr ("Limiteur intégré"), 495, 330, 130, 18, juce::Justification::centred);

        // Voyant du gate
        const bool gateOn = proc.param ("gate") > LeadDSP::gateOffDb + .5f;
        const bool open = proc.gateOpen.load();
        g.setColour (! gateOn ? colours::track : open ? colours::open : juce::Colour (0xff7a3b4f));
        g.fillEllipse (94, 358, 9, 9);
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.f));
        g.drawText (! gateOn ? fr ("éteint") : open ? juce::String ("ouvert") : fr ("fermé"), 108, 354, 70, 16, juce::Justification::left);

        // Voyant du limiteur
        const float limDb = juce::Decibels::gainToDecibels (proc.limiterGain.load(), -60.f);
        const bool limiting = limDb < -0.1f;
        auto led = juce::Rectangle<float> (530, 354, 60, 18);
        g.setColour (limiting ? juce::Colour (0xff3a1f2a) : juce::Colour (0xff222327));
        g.fillRoundedRectangle (led, 4);
        g.setColour (limiting ? colours::accent : colours::track);
        g.drawRoundedRectangle (led, 4, 1);
        g.setColour (limiting ? juce::Colour (0xffff8fb4) : colours::muted);
        g.drawText ("LIMIT", led, juce::Justification::centred);

        // Vumètres
        g.setColour (colours::track);
        g.drawHorizontalLine (436, 0, (float) getWidth());
        const float grDb = proc.compGrDb.load();
        drawMeter (g, "IN", 40, toMeter (proc.inLevel.load()), juce::Colour (0xffb6b7bf));
        drawMeter (g, "COMP", 250, juce::jlimit (0.f, 1.f, -grDb / 24.f), colours::accent);
        drawMeter (g, "OUT", 460, toMeter (proc.outLevel.load()), colours::ink);
        g.setColour (juce::Colour (0xff6e6f77));
        g.setFont (juce::FontOptions (11.f));
        g.drawText ("v" JucePlugin_VersionString "  /  by @sevyondatracks", getWidth() - 230, getHeight() - 20, 214, 16, juce::Justification::centredRight);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // Réduction de gain en direct, sur l'anneau intérieur du gros bouton.
        const float grDb = proc.compGrDb.load();
        const float amount = juce::jlimit (0.f, 1.f, -grDb / 24.f);
        const auto c = compKnob.getBounds().toFloat().getCentre();
        if (amount > 0.005f)
        {
            juce::Path p;
            const float half = amount * juce::MathConstants<float>::pi * 0.75f;
            p.addCentredArc (c.x, c.y, 103, 103, 0, -half, half, true);
            g.setColour (colours::accentSoft);
            g.strokePath (p, juce::PathStrokeType (3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (44.f, juce::Font::bold));
        g.drawText (juce::String (juce::roundToInt (compKnob.getValue())), juce::Rectangle<float> (200, 40).withCentre (c.translated (0, -4)), juce::Justification::centred);
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (13.f));
        g.drawText ("%", juce::Rectangle<float> (60, 18).withCentre (c.translated (0, 28)), juce::Justification::centred);
        g.setColour (colours::accent);
        g.setFont (juce::FontOptions (13.f));
        g.drawText (fr ("Réduction : ") + juce::String (grDb, 1) + " dB", 0, 408, getWidth(), 18, juce::Justification::centred);

        g.setColour (colours::ink);
        g.setFont (juce::FontOptions (16.f, juce::Font::bold));
        const float gate = (float) gateKnob.getValue();
        g.drawText (gate <= LeadDSP::gateOffDb + .5f ? "Off" : juce::String (juce::roundToInt (gate)) + " dB",
                    gateKnob.getBounds(), juce::Justification::centred);
        const float gain = (float) gainKnob.getValue();
        g.drawText ((gain > 0.05f ? "+" : "") + juce::String (gain, 1) + " dB", gainKnob.getBounds(), juce::Justification::centred);
    }

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    void setupKnob (juce::Slider& k, const juce::String& name, double resetValue)
    {
        k.setName (name);
        k.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        k.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        k.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        k.setDoubleClickReturnValue (true, resetValue);
        k.setLookAndFeel (&look);
        addAndMakeVisible (k);
    }

    static float toMeter (float peak) { return juce::jlimit (0.f, 1.f, (juce::Decibels::gainToDecibels (peak, -60.f) + 60.f) / 60.f); }

    void drawMeter (juce::Graphics& g, const juce::String& label, int x, float value, juce::Colour colour)
    {
        g.setColour (colours::muted);
        g.setFont (juce::FontOptions (11.f));
        g.drawText (label, x, 452, 40, 16, juce::Justification::left);
        auto bar = juce::Rectangle<float> ((float) x + 42, 456, 140, 8);
        g.setColour (juce::Colour (0xff2a2b30));
        g.fillRoundedRectangle (bar, 3);
        g.setColour (colour);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * value), 3);
    }

    void timerCallback() override { repaint(); }

    LeadProcessor& proc;
    LeadLook look;
    juce::Slider compKnob, gateKnob, gainKnob;
    juce::TextButton bypass { "Bypass" };
    juce::Image logo;
    std::unique_ptr<SliderAttachment> compAttach, gateAttach, gainAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> bypassAttach;
};

juce::AudioProcessorEditor* LeadProcessor::createEditor()
{
    return swell::LicenceGuard::attach (new ResizableEditor (*this, new LeadEditor (*this)));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LeadProcessor(); }
