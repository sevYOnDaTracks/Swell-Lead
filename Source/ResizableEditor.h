#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

class ResizableEditor final : public juce::AudioProcessorEditor
{
public:
    ResizableEditor(juce::AudioProcessor& processor, juce::AudioProcessorEditor* editor)
        : AudioProcessorEditor(processor), content(editor)
    {
        addAndMakeVisible(*content);
        setResizable(true, true);
        updateLimits();
        setSize(content->getWidth(), content->getHeight());
    }

    void resized() override
    {
        if (content && getWidth() > 0)
            content->setTransform(juce::AffineTransform::scale(float(getWidth()) / content->getWidth()));
    }

    void childBoundsChanged(juce::Component* child) override
    {
        if (child == content.get() && !updating && getWidth() > 0)
        {
            juce::ScopedValueSetter<bool> guard(updating, true);
            updateLimits();
            setSize(getWidth(), juce::roundToInt(double(getWidth()) * content->getHeight() / content->getWidth()));
        }
    }

private:
    void updateLimits()
    {
        const int w = content->getWidth(), h = content->getHeight();
        getConstrainer()->setFixedAspectRatio(double(w) / h);
        setResizeLimits(w / 2, h / 2, 2 * w, 2 * h);
    }
    std::unique_ptr<juce::AudioProcessorEditor> content;
    bool updating = false;
};
