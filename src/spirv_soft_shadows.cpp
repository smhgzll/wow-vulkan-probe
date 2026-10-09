#include "spirv_soft_shadows.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace depth_probe::softshadow {
namespace {

enum : uint16_t {
    OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15, OpCapability = 17, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeImage = 25, OpTypeSampledImage = 27, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43,
    OpConstantComposite = 44, OpFunction = 54, OpVariable = 59, OpLoad = 61, OpAccessChain = 65, OpDecorate = 71,
    OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeExtract = 81, OpSampledImage = 86, OpImageSampleDrefExplicitLod = 90,
    OpImageFetch = 95, OpImage = 100, OpImageQuerySizeLod = 103, OpConvertFToS = 110, OpConvertSToF = 111, OpISub = 130,
    OpFAdd = 129, OpFSub = 131, OpFMul = 133, OpFDiv = 136, OpVectorTimesScalar = 142, OpLogicalAnd = 167, OpSelect = 169,
    OpFOrdEqual = 180, OpFOrdLessThan = 184, OpFOrdGreaterThan = 186,
};
enum : uint32_t { GlslFloor = 8, GlslFMax = 40, GlslFClamp = 43, GlslSClamp = 45, CapImageQuery = 50, ExecutionFragment = 4,
                  StorageUniform = 2, DecorationBinding = 33, DecorationSet = 34, DecorationOffset = 35 };

struct Insn { size_t at; uint16_t op; uint16_t words; };

uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

class Emitter {
public:
    explicit Emitter(uint32_t bound) : next(bound) {}
    uint32_t id() { return next++; }
    void op(std::vector<uint32_t>& to, uint16_t code, std::initializer_list<uint32_t> operands) {
        to.push_back(uint32_t(operands.size() + 1) << 16 | code);
        to.insert(to.end(), operands);
    }
    uint32_t next;
};

}  // namespace

Result patch(const uint32_t* w, size_t n, const Params& p) {
    Result r;
    if (n < 5 || w[0] != 0x07230203) { r.why = "not SPIR-V"; return r; }
    std::vector<Insn> insns;
    for (size_t i = 5; i < n;) {
        const uint16_t words = uint16_t(w[i] >> 16), op = uint16_t(w[i] & 0xffff);
        if (!words || i + words > n) { r.why = "malformed"; return r; }
        insns.push_back({i, op, words});
        i += words;
    }
    // Declarations.
    bool fragment = false, imageQuery = false;
    uint32_t glsl = 0, tBool = 0, tFloat = 0, tInt = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vectors;   // (component type, count) -> type
    std::unordered_map<uint32_t, uint32_t> sampledImageOf;       // OpTypeSampledImage -> its image type
    std::unordered_map<uint32_t, uint32_t> binding, descriptorSet;   // variable -> binding, set
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> pointers;       // (storage class, pointee) -> pointer type
    std::unordered_map<uint32_t, uint32_t> pointee;                   // pointer type -> pointee
    std::unordered_map<uint32_t, std::vector<uint32_t>> members;      // struct -> member types
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> memberOffset;   // (struct, member) -> byte offset
    std::vector<std::pair<uint32_t, uint32_t>> uniforms;              // (variable, pointer type) in the Uniform storage class
    std::unordered_map<uint32_t, uint32_t> loadedFrom;            // load / access chain result -> variable
    std::unordered_map<uint32_t, uint32_t> sampledVar, sampledType;   // OpSampledImage result -> image variable, its type
    size_t firstFunction = n;
    for (const auto& in : insns) {
        const uint32_t* a = w + in.at + 1;
        switch (in.op) {
        case OpCapability: imageQuery |= a[0] == CapImageQuery; break;
        case OpExtInstImport: if (!std::strncmp(reinterpret_cast<const char*>(a + 1), "GLSL.std.450", 12)) glsl = a[0]; break;
        case OpEntryPoint: fragment |= a[0] == ExecutionFragment; break;
        case OpTypeBool: tBool = a[0]; break;
        case OpTypeFloat: if (a[1] == 32) tFloat = a[0]; break;
        case OpTypeInt: if (a[1] == 32 && a[2] == 1) tInt = a[0]; break;
        case OpTypeVector: vectors[{a[1], a[2]}] = a[0]; break;
        case OpTypeSampledImage: sampledImageOf[a[0]] = a[1]; break;
        case OpDecorate:
            if (in.words >= 4 && a[1] == DecorationBinding) binding[a[0]] = a[2];
            if (in.words >= 4 && a[1] == DecorationSet) descriptorSet[a[0]] = a[2];
            break;
        case OpMemberDecorate: if (in.words >= 5 && a[2] == DecorationOffset) memberOffset[{a[0], a[1]}] = a[3]; break;
        case OpTypeStruct: members[a[0]].assign(a + 1, a + in.words - 1); break;
        case OpTypePointer: pointers.try_emplace({a[1], a[2]}, a[0]); pointee[a[0]] = a[2]; break;
        case OpVariable: if (in.at < firstFunction && a[2] == StorageUniform) uniforms.push_back({a[1], a[0]}); break;
        case OpFunction: firstFunction = std::min(firstFunction, in.at); break;
        case OpLoad: case OpAccessChain: loadedFrom[a[1]] = loadedFrom.count(a[2]) ? loadedFrom[a[2]] : a[2]; break;
        case OpSampledImage: sampledVar[a[1]] = loadedFrom.count(a[2]) ? loadedFrom[a[2]] : 0; sampledType[a[1]] = a[0]; break;
        default: break;
        }
    }
    if (!fragment) { r.why = "not a fragment shader"; return r; }
    if (!glsl || !tFloat) { r.why = "no GLSL.std.450 / float"; return r; }
    // The cascades: images sampled 5+ times with a depth compare at an explicit lod.
    std::map<uint32_t, uint32_t> samplesPerVar;
    for (const auto& in : insns)
        if (in.op == OpImageSampleDrefExplicitLod) ++samplesPerVar[sampledVar[w[in.at + 3]]];
    std::vector<uint32_t> cascades;
    for (auto [var, count] : samplesPerVar) if (var && count >= 5) cascades.push_back(var);
    uint32_t indoorVar = 0;   // or an indoor shadow shader: every depth compare from one image, 8+ of them
    if (cascades.size() != 4) {
        uint32_t total = 0;
        for (auto [var, count] : samplesPerVar) total += count;
        for (auto [var, count] : samplesPerVar) if (var && count >= 8 && count == total) indoorVar = var;
        if (!indoorVar) { r.why = "not 4 cascade images (" + std::to_string(cascades.size()) + ") nor one indoor shadow map"; return r; }
        cascades.clear();
    }
    std::sort(cascades.begin(), cascades.end(), [&](uint32_t a, uint32_t b) { return binding[a] < binding[b]; });
    auto cascadeOf = [&](uint32_t var) { return int(std::find(cascades.begin(), cascades.end(), var) - cascades.begin()); };

    Emitter e(w[3]);
    std::vector<uint32_t> globals;
    auto type = [&](uint32_t& id, std::initializer_list<uint32_t> operands, uint16_t code) {
        if (id) return id;
        id = e.id();
        std::vector<uint32_t> ops{id};
        ops.insert(ops.end(), operands);
        globals.push_back(uint32_t(ops.size() + 1) << 16 | code);
        globals.insert(globals.end(), ops.begin(), ops.end());
        return id;
    };
    type(tBool, {}, OpTypeBool);
    type(tInt, {32, 1}, OpTypeInt);
    uint32_t tV2 = vectors.count({tFloat, 2}) ? vectors[{tFloat, 2}] : 0, tV4 = vectors.count({tFloat, 4}) ? vectors[{tFloat, 4}] : 0;
    uint32_t tV2i = vectors.count({tInt, 2}) ? vectors[{tInt, 2}] : 0;
    type(tV2, {tFloat, 2}, OpTypeVector);
    type(tV4, {tFloat, 4}, OpTypeVector);
    type(tV2i, {tInt, 2}, OpTypeVector);
    std::map<uint32_t, uint32_t> floats;
    auto f = [&](float v) {
        auto [it, fresh] = floats.try_emplace(bits(v), 0);
        if (fresh) { it->second = e.id(); e.op(globals, OpConstant, {tFloat, it->second, bits(v)}); }
        return it->second;
    };
    const uint32_t i0 = e.id(), i1 = e.id(), zero2i = e.id(), one2i = e.id(), one2f = e.id();
    e.op(globals, OpConstant, {tInt, i0, 0});
    e.op(globals, OpConstant, {tInt, i1, 1});
    e.op(globals, OpConstantComposite, {tV2i, zero2i, i0, i0});
    e.op(globals, OpConstantComposite, {tV2i, one2i, i1, i1});
    e.op(globals, OpConstantComposite, {tV2, one2f, f(1), f(1)});
    auto v2 = [&](float x, float y) { const uint32_t id = e.id(); e.op(globals, OpConstantComposite, {tV2, id, f(x), f(y)}); return id; };

    // Live settings: DXVK's D3D9SharedPS, recognised by its layout (8 stages of 48 bytes: vec4, vec2, vec2, float, float) with
    // stage 7's words where expected (see liveOffset), wherever it is bound - the layer writes to the set / binding found here.
    uint32_t shared = 0, pUniformFloat = 0, pUniformV2 = 0, iMagic = 0, iSize = 0, iFade = 0, iIndoor = 0;
    for (auto [var, pointer] : uniforms) {
        const uint32_t st = pointee[pointer];
        if (members[st].size() != 40) continue;
        auto at = [&](uint32_t m) { auto it = memberOffset.find({st, m}); return it == memberOffset.end() ? ~0u : it->second; };
        bool layout = true;
        for (uint32_t stage = 0; stage < 8; ++stage) {
            const uint32_t m = stage * 5, base = stage * 48;
            layout &= members[st][m] == tV4 && members[st][m + 1] == tV2 && members[st][m + 2] == tV2 && members[st][m + 3] == tFloat &&
                      members[st][m + 4] == tFloat && at(m) == base && at(m + 1) == base + 16 && at(m + 2) == base + 24 &&
                      at(m + 3) == base + 32 && at(m + 4) == base + 36;
        }
        if (!layout || at(37) != liveOffset) continue;
        if (!descriptorSet.count(var) || !binding.count(var)) { r.liveWhy = "D3D9SharedPS without a set / binding"; continue; }
        shared = var;
        r.liveSet = descriptorSet[var];
        r.liveBinding = binding[var];
    }
    if (!shared && r.liveWhy.empty()) {
        r.liveWhy = "no D3D9SharedPS-shaped buffer:";
        for (auto [var, pointer] : uniforms)
            r.liveWhy += " s" + std::to_string(descriptorSet.count(var) ? descriptorSet[var] : 99) + "/b" +
                         std::to_string(binding.count(var) ? binding[var] : 99) + " " + std::to_string(members[pointee[pointer]].size()) + " members";
    }
    if (shared) {
        auto pointer = [&](uint32_t type) {
            if (auto it = pointers.find({StorageUniform, type}); it != pointers.end()) return it->second;
            const uint32_t id = e.id();
            e.op(globals, OpTypePointer, {id, StorageUniform, type});
            return id;
        };
        pUniformFloat = pointer(tFloat);
        pUniformV2 = pointer(tV2);
        iMagic = e.id(); iSize = e.id(); iFade = e.id(); iIndoor = e.id();
        e.op(globals, OpConstant, {tInt, iIndoor, 36});
        e.op(globals, OpConstant, {tInt, iMagic, 37});
        e.op(globals, OpConstant, {tInt, iSize, 38});
        e.op(globals, OpConstant, {tInt, iFade, 39});
        r.live = true;
    }

    // Taps per cascade, in program order (each tap takes another part of the pattern).
    std::map<int, int> tapsIn, tapIndex;
    if (!indoorVar)
        for (const auto& in : insns) if (in.op == OpImageSampleDrefExplicitLod) { const int k = cascadeOf(sampledVar[w[in.at + 3]]); if (k < 4) ++tapsIn[k]; }

    std::vector<uint32_t> body;
    body.reserve(n * 2);
    for (const auto& in : insns) {
        if (in.at < firstFunction) continue;
        const uint32_t* a = w + in.at + 1;
        if (indoorVar) {   // an indoor shadow shader: the game's sample, then 1 - strength (1 - it)
            if (in.op != OpImageSampleDrefExplicitLod || sampledVar[a[2]] != indoorVar) { body.insert(body.end(), w + in.at, w + in.at + in.words); continue; }
            auto op = [&](uint16_t code, std::initializer_list<uint32_t> operands) { e.op(body, code, operands); };
            uint32_t strength = f(p.indoorStrength);
            if (shared) {
                const uint32_t cm = e.id(), m2 = e.id(), magic = e.id(), ci = e.id(), i2 = e.id(), indoorL = e.id(), live = e.id(), chosen = e.id();
                op(OpAccessChain, {pUniformV2, cm, shared, iMagic});
                op(OpLoad, {tV2, m2, cm});
                op(OpCompositeExtract, {tFloat, magic, m2, 0});
                op(OpAccessChain, {pUniformV2, ci, shared, iIndoor});
                op(OpLoad, {tV2, i2, ci});
                op(OpCompositeExtract, {tFloat, indoorL, i2, 0});
                op(OpFOrdEqual, {tBool, live, magic, f(liveMagic)});
                op(OpSelect, {tFloat, chosen, live, indoorL, strength});
                strength = chosen;
            }
            const uint32_t original = e.id(), open = e.id(), dim = e.id();
            std::vector<uint32_t> copy(w + in.at, w + in.at + in.words);
            copy[2] = original;
            body.insert(body.end(), copy.begin(), copy.end());
            op(OpFSub, {a[0], open, f(1), original});
            op(OpFMul, {a[0], dim, strength, open});
            op(OpFSub, {a[0], a[1], f(1), dim});
            ++r.taps;
            r.indoor = true;
            continue;
        }
        const int k = in.op == OpImageSampleDrefExplicitLod ? cascadeOf(sampledVar[a[2]]) : 4;
        if (k >= 4 || in.words < 7 || a[5] != 2u /* Lod only */) { body.insert(body.end(), w + in.at, w + in.at + in.words); continue; }
        const uint32_t resultType = a[0], result = a[1], si = a[2], coord = a[3], dref = a[4], lod = a[6];
        const uint32_t imageType = sampledImageOf[sampledType[si]];
        const int t = tapIndex[k]++, taps = tapsIn[k];
        auto op = [&](uint16_t code, std::initializer_list<uint32_t> operands) { e.op(body, code, operands); };
        // The settings: live (D3D9SharedPS, when the layer wrote them) or baked.
        uint32_t size = f(p.size), fade = f(p.fade), fadeEnd = f(p.fadeEnd);
        if (shared) {
            const uint32_t cm = e.id(), m2 = e.id(), magic = e.id(), endL = e.id(), cs = e.id(), sizeL = e.id(), cf = e.id(), fadeL = e.id(), live = e.id();
            op(OpAccessChain, {pUniformV2, cm, shared, iMagic});
            op(OpLoad, {tV2, m2, cm});
            op(OpCompositeExtract, {tFloat, magic, m2, 0});
            op(OpCompositeExtract, {tFloat, endL, m2, 1});
            op(OpAccessChain, {pUniformFloat, cs, shared, iSize});
            op(OpLoad, {tFloat, sizeL, cs});
            op(OpAccessChain, {pUniformFloat, cf, shared, iFade});
            op(OpLoad, {tFloat, fadeL, cf});
            op(OpFOrdEqual, {tBool, live, magic, f(liveMagic)});
            const uint32_t s2 = e.id(), f2 = e.id(), e2 = e.id();
            op(OpSelect, {tFloat, s2, live, sizeL, size});
            op(OpSelect, {tFloat, f2, live, fadeL, fade});
            op(OpSelect, {tFloat, e2, live, endL, fadeEnd});
            size = s2; fade = f2; fadeEnd = e2;
        }
        const uint32_t img = e.id(), sz = e.id(), sizef = e.id(), inv = e.id(), maxT = e.id(), uv = e.id(), drefB = e.id(), width = e.id(), perYard = e.id();
        op(OpImage, {imageType, img, si});
        op(OpImageQuerySizeLod, {tV2i, sz, img, i0});
        op(OpConvertSToF, {tV2, sizef, sz});
        op(OpFDiv, {tV2, inv, one2f, sizef});
        op(OpISub, {tV2i, maxT, sz, one2i});
        op(OpVectorShuffle, {tV2, uv, coord, coord, 0, 1});
        op(OpFSub, {tFloat, drefB, dref, f(5e-5f)});                 // .2 yd: a surface is not its own blocker
        op(OpCompositeExtract, {tFloat, width, sizef, 0});
        op(OpFMul, {tFloat, perYard, size, f(.05f / p.cascadeYards[k])});   // penumbra (texels / map width) per yard
        // Blocker search: 2 raw depths around the tap, within the widest penumbra from 20 yd (1.5 .. 24 texels).
        const uint32_t searchRaw0 = e.id(), searchRaw = e.id(), search = e.id();
        op(OpFMul, {tFloat, searchRaw0, width, perYard});
        op(OpFMul, {tFloat, searchRaw, searchRaw0, f(20)});
        op(OpExtInst, {tFloat, search, glsl, GlslFClamp, searchRaw, f(1.5f), f(24)});
        uint32_t sum = 0, count = 0;
        for (int b = 0; b < 2; ++b) {
            const int s = t * 2 + b, of = taps * 2;
            const float angle = float(s) * 2.39996323f + float(k), radius = std::sqrt((float(s) + .5f) / float(of));
            const uint32_t pat = v2(radius * std::cos(angle), radius * std::sin(angle));
            const uint32_t off = e.id(), offUv = e.id(), uvb = e.id(), scaled = e.id(), fl = e.id(), ti = e.id(), tc = e.id(), d4 = e.id(), d = e.id(),
                           is = e.id(), db = e.id(), cb = e.id();
            op(OpVectorTimesScalar, {tV2, off, pat, search});
            op(OpFMul, {tV2, offUv, off, inv});
            op(OpFAdd, {tV2, uvb, uv, offUv});
            op(OpFMul, {tV2, scaled, uvb, sizef});
            op(OpExtInst, {tV2, fl, glsl, GlslFloor, scaled});
            op(OpConvertFToS, {tV2i, ti, fl});
            op(OpExtInst, {tV2i, tc, glsl, GlslSClamp, ti, zero2i, maxT});
            op(OpImageFetch, {tV4, d4, img, tc, 2, i0});
            op(OpCompositeExtract, {tFloat, d, d4, 0});
            op(OpFOrdLessThan, {tBool, is, d, drefB});
            op(OpSelect, {tFloat, db, is, d, f(0)});
            op(OpSelect, {tFloat, cb, is, f(1), f(0)});
            if (!b) { sum = db; count = cb; }
            else { const uint32_t s2 = e.id(), c2 = e.id(); op(OpFAdd, {tFloat, s2, sum, db}); op(OpFAdd, {tFloat, c2, count, cb}); sum = s2; count = c2; }
        }
        // Penumbra from the receiver-blocker distance.
        const uint32_t countM = e.id(), avg = e.id(), gap = e.id(), dist = e.id(), penRaw0 = e.id(), penRaw = e.id(), pen = e.id();
        op(OpExtInst, {tFloat, countM, glsl, GlslFMax, count, f(1e-3f)});
        op(OpFDiv, {tFloat, avg, sum, countM});
        op(OpFSub, {tFloat, gap, dref, avg});
        op(OpFMul, {tFloat, dist, gap, f(p.depthYards)});
        op(OpFMul, {tFloat, penRaw0, dist, perYard});
        op(OpFMul, {tFloat, penRaw, penRaw0, width});
        op(OpExtInst, {tFloat, pen, glsl, GlslFClamp, penRaw, f(1), f(24)});
        // Filter: 3 depth compares over the penumbra.
        uint32_t acc = 0;
        for (int q = 0; q < 3; ++q) {
            const int s = t * 3 + q, of = taps * 3;
            const float angle = float(s) * 2.39996323f + 1.f + float(k), radius = std::sqrt((float(s) + .5f) / float(of));
            const uint32_t pat = v2(radius * std::cos(angle), radius * std::sin(angle));
            const uint32_t off = e.id(), offUv = e.id(), uvf = e.id(), sample = e.id();
            op(OpVectorTimesScalar, {tV2, off, pat, pen});
            op(OpFMul, {tV2, offUv, off, inv});
            op(OpFAdd, {tV2, uvf, uv, offUv});
            op(OpImageSampleDrefExplicitLod, {resultType, sample, si, uvf, dref, 2, lod});
            if (!q) acc = sample;
            else { const uint32_t a2 = e.id(); op(OpFAdd, {tFloat, a2, acc, sample}); acc = a2; }
        }
        const uint32_t soft = e.id();
        op(OpFMul, {tFloat, soft, acc, f(1.f / 3.f)});
        // Lifted with the distance from the caster: fade x clamp((dist - fadeStart) / (fadeEnd - fadeStart), 0, 1).
        const uint32_t span = e.id(), spanM = e.id(), from = e.id(), share = e.id(), shareC = e.id(), lifted = e.id(), open = e.id(), lift = e.id(), faded = e.id();
        op(OpFSub, {tFloat, span, fadeEnd, f(p.fadeStart)});
        op(OpExtInst, {tFloat, spanM, glsl, GlslFMax, span, f(.1f)});
        op(OpFSub, {tFloat, from, dist, f(p.fadeStart)});
        op(OpFDiv, {tFloat, share, from, spanM});
        op(OpExtInst, {tFloat, shareC, glsl, GlslFClamp, share, f(0), f(1)});
        op(OpFMul, {tFloat, lifted, shareC, fade});
        op(OpFSub, {tFloat, open, f(1), soft});
        op(OpFMul, {tFloat, lift, open, lifted});
        op(OpFAdd, {tFloat, faded, soft, lift});
        // The game's own sample, kept where no blocker was found or the size is 0; the tap's result is the choice.
        const uint32_t original = e.id(), has = e.id(), on = e.id(), use = e.id();
        std::vector<uint32_t> copy(w + in.at, w + in.at + in.words);
        copy[2] = original;
        body.insert(body.end(), copy.begin(), copy.end());
        op(OpFOrdGreaterThan, {tBool, has, count, f(0)});
        op(OpFOrdGreaterThan, {tBool, on, size, f(0)});
        op(OpLogicalAnd, {tBool, use, has, on});
        op(OpSelect, {resultType, result, use, faded, original});
        ++r.taps;
    }
    // Header + declarations (ImageQuery after the last capability when missing) + the new globals + the functions.
    r.code.assign(w, w + 5);
    size_t lastCapability = 0;
    for (const auto& in : insns) if (in.op == OpCapability) lastCapability = in.at;
    for (const auto& in : insns) {
        if (in.at >= firstFunction) break;
        r.code.insert(r.code.end(), w + in.at, w + in.at + in.words);
        if (in.at == lastCapability && !imageQuery) { r.code.push_back(2u << 16 | OpCapability); r.code.push_back(CapImageQuery); }
    }
    r.code.insert(r.code.end(), globals.begin(), globals.end());
    r.code.insert(r.code.end(), body.begin(), body.end());
    r.code[3] = e.next;
    return r;
}

}  // namespace depth_probe::softshadow
