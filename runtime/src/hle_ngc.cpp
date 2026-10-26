// nn::ngc (the profanity filter) at the API level: a filter with an empty word list. First needed by ACNH before 3.0.2
// (1.0.0, 2.0.7), whose ProfanityFilter::Initialize mounts the console's NgWord system data (0100000000000806)
// through fsp-srv at boot; there is no such archive here and the SDK read a null storage (docs/VERSIONS.md). Names,
// messages and designs are passed through unchanged, as on a console whose filter found nothing.
#include <nx/runtime.h>

namespace nx {

namespace {

constexpr uint32_t kOk = 0;

// ProfanityFilter::Initialize(void* work, size_t size, bool check_desired_language) / SetMaskMode(MaskMode)
void ok(CPU& c) { c.x[0] = kOk; }

// ProfanityFilter::CheckProfanityWords(uint32_t* results, uint32_t pattern, const char16_t** words, size_t count):
// one bit set per pattern a word matched; none here.
void CheckProfanityWords(CPU& c) {
    for (uint64_t k = 0; c.x[1] && k < c.x[4]; k++) st<uint32_t>(c.x[1] + 4 * k, 0);
    c.x[0] = kOk;
}

// ProfanityFilter::MaskProfanityWordsInText(int* masked, char16_t* text, uint32_t pattern): nothing masked.
void MaskProfanityWordsInText(CPU& c) {
    if (c.x[1]) st<int32_t>(c.x[1], 0);
    c.x[0] = kOk;
}

}  // namespace

// x0 is `this` for the members: the results and arguments start at x1.
NX_HLE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ok);
NX_HLE("_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", ok);
NX_HLE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", CheckProfanityWords);
NX_HLE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", MaskProfanityWordsInText);

}  // namespace nx
