#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** Minimal ARA document controller: default playback/editor renderers, empty persistence. */
class AREDocumentController final : public juce::ARADocumentControllerSpecialisation
{
public:
    using juce::ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

    juce::ReadWriteLock& getProcessLock() noexcept { return processBlockLock; }

    static AREDocumentController* fromDocumentController (ARA::PlugIn::DocumentController* dc) noexcept
    {
        if (dc == nullptr)
            return nullptr;

        return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<AREDocumentController> (dc);
    }

protected:
    bool doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                     const juce::ARARestoreObjectsFilter*) noexcept override;

    bool doStoreObjectsToStream (juce::ARAOutputStream& output,
                                 const juce::ARAStoreObjectsFilter*) noexcept override;

    void willBeginEditing (juce::ARADocument*) override { processBlockLock.enterWrite(); }
    void didEndEditing (juce::ARADocument*) override { processBlockLock.exitWrite(); }

private:
    juce::ReadWriteLock processBlockLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AREDocumentController)
};
