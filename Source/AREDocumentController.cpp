#include <JuceHeader.h>
#include "AREDocumentController.h"

bool AREDocumentController::doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                                        const juce::ARARestoreObjectsFilter*) noexcept
{
    [[maybe_unused]] const auto storedMods = input.readInt64();
    return ! input.failed();
}

bool AREDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output,
                                                    const juce::ARAStoreObjectsFilter*) noexcept
{
    return output.writeInt64 (0);
}

#if JucePlugin_Enable_ARA
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<AREDocumentController>();
}
#endif
