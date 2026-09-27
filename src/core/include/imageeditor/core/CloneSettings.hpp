#pragma once

namespace imageeditor::core {

enum class CloneMode { Stamp, Heal, SpotHeal };
enum class CloneSampleSource { SourceLayer, CurrentAndBelow, AllVisible };

// Session state. Sampling an identified layer reads its intrinsic color while
// respecting visibility and crop; rendered references intentionally bake the
// visible adjustment/opacity/blend result into the sampled appearance.
struct CloneSettings {
    CloneMode mode {CloneMode::Stamp};
    CloneSampleSource source {CloneSampleSource::SourceLayer};
    bool aligned {true};
    double adaptation {.8};
    friend bool operator==(const CloneSettings&, const CloneSettings&) = default;
};

} // namespace imageeditor::core
