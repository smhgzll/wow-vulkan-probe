#include "draw_segments.hpp"

namespace depth_probe {
bool draw_class_solid(DrawClass c) {
    return c == DrawClass::Sky || c == DrawClass::Opaque || c == DrawClass::AlphaTest || c == DrawClass::DepthOnly;
}
bool draw_class_blended(DrawClass c) {
    return c == DrawClass::Transparent || c == DrawClass::Water || c == DrawClass::Additive || c == DrawClass::Particle;
}

void SegmentFrame::add(DrawClass cls, uint32_t passOrdinal, bool passStart, bool writesDepth, bool blendedDraw) {
    const uint32_t i = draws++;
    const auto k = size_t(cls);
    if (!count[k]++) first[k] = i;
    last[k] = i;
    auto split = [&](SegmentSplit& s, bool starts) {
        if (!s.found && starts) { s.found = true; s.index = i; s.passOrdinal = passOrdinal; s.atPassStart = passStart; }
    };
    const bool cutout = cls == DrawClass::AlphaTest && blendedDraw;
    const bool blended = draw_class_blended(cls) || cutout;
    // Before recording this draw into either split: count what lies on the wrong side of an already found split - solid
    // draws that write colour (a depth-only draw after the fog changes no pixel).
    const bool colourSolid = draw_class_solid(cls) && cls != DrawClass::DepthOnly && !cutout;
    for (SegmentSplit* s : {&anyBlended, &alphaBlended, &phaseStart}) {
        if (s->found && colourSolid) ++s->solidsAfter;
    }
    split(anyBlended, blended);
    split(alphaBlended, cls == DrawClass::Transparent || cls == DrawClass::Water || cutout);
    split(phaseStart, cls == DrawClass::Water || ((cls == DrawClass::Transparent || cutout) && writesDepth));
    for (SegmentSplit* s : {&alphaBlended, &phaseStart})
        if (blended && !writesDepth && !s->found) ++s->blendedBefore;
}
}
