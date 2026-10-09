#include <map>
#include "discovery_internal.hpp"

namespace depth_probe::discovery {
uint64_t hash_bytes(const void* data,size_t size,uint64_t hash) {
    const auto* p=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;++i)hash=(hash^p[i])*1099511628211ull;
    return hash;
}
void hash_value(uint64_t& hash,uint64_t value) {
    for(unsigned i=0;i<8;++i){const auto byte=uint8_t(value);hash=hash_bytes(&byte,1,hash);value>>=8;}
}
std::string hex(uint64_t value){std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<value;return s.str();}
Shader inspect_spirv(const uint32_t* words,size_t size) {
    Shader out;
    if(!words || size<20 || size%4 || size>4*1024*1024 || words[0]!=0x07230203 ||
       words[3]==0 || words[3]>262144)return out;
    out.hash=hash_bytes(words,size);out.words.assign(words,words+size/4);
    struct Id {uint32_t root=0,set=UINT32_MAX,binding=UINT32_MAX,storage=UINT32_MAX;};
    std::vector<Id> ids(words[3]);
    std::vector<uint32_t> stores,sampled,dref;
    out.isolationSafe=true;out.replaySafe=true;bool discard=false,sampleMask=false;
    for(size_t i=5;i<size/4;) {
        uint32_t count=words[i]>>16,op=words[i]&65535;
        if(!count || count>size/4-i){out.isolationSafe=false;return out;}
        const auto* w=words+i;
        // Early tests can count fragments later discarded. Subgroup/helper
        // observations may change under a one-pixel scissor. Never replay them.
        if(op==16 && count>=3 && w[2]==9)out.earlyTests=true;
        if(op==252 || op==4416 || op==5380)discard=true;
        if(op==71 && count>=4 && w[2]==11 && w[3]==20)sampleMask=true;
        if((op==71 && count>=4 && w[2]==11 && w[3]==23) || (op==25 && count>=4 && w[3]==6) ||
           (op>=333 && op<=366) || (op>=4096 && op!=4416 && op!=5380))out.replaySafe=false;
        auto valid=[&](unsigned n){return n<count && w[n]<ids.size();};
        if(op==71 && count>=4 && valid(1)) { // OpDecorate Binding / DescriptorSet
            if(w[2]==33)ids[w[1]].binding=w[3];
            if(w[2]==34)ids[w[1]].set=w[3];
        }
        if(op==32 && count>=4 && valid(1))ids[w[1]].storage=w[2]; // pointer address space
        // A Function pointer parameter cannot alias a storage buffer. Preserve
        // its proven address space so helper-local writes are not side effects.
        if(op==55 && count>=3 && valid(1) && valid(2) && ids[w[1]].storage!=UINT32_MAX) {
            ids[w[2]].root=w[2];ids[w[2]].storage=ids[w[1]].storage;
        }
        if(op==59 && count>=4 && valid(2)) {ids[w[2]].root=w[2];ids[w[2]].storage=w[3];}
        // Load, access-chain, copy-object, sampled-image and image preserve a
        // direct resource root. Phi/function/descriptor indexing ambiguity is
        // deliberately left unresolved; this is not a full SPIR-V interpreter.
        if((op==61 || op==65 || op==66 || op==67 || op==83 || op==86 || op==100) && count>=4 && valid(2) && valid(3))
            ids[w[2]].root=ids[w[3]].root;
        if(op==62 && count>=3 && valid(1))stores.push_back(w[1]);
        if(op==63 || op==64)out.isolationSafe=false; // memory copy needs deeper alias analysis
        if(op>=87 && op<=98 && count>=4 && valid(3)) {
            const bool isDref=op==89 || op==90 || op==93 || op==94 || op==97;
            if(op==95 || op==98)++out.fetches;
            else if(op==96 || op==97)++out.gathers;
            else ++out.samples;
            if(isDref){++out.drefs;dref.push_back(w[3]);}
            sampled.push_back(w[3]);
        }
        // Never suppress a shader with an image write, atomic, geometry stream,
        // barrier, or unanalysed extension instruction. Demote-to-helper is a
        // fragment coverage operation, not a storage side effect.
        if(op==99 || (op>=218 && op<=242) || (op>=4096 && op!=4416 && op!=5380 && op!=5381))out.isolationSafe=false;
        i+=count;
    }
    for(auto p:stores) {
        const auto root=ids[p].root;
        if(!root || (ids[root].storage!=3 && ids[root].storage!=6 && ids[root].storage!=7))out.isolationSafe=false;
    }
    for(uint32_t i=1;i<ids.size();++i) {
        const auto& value=ids[i];
        if(value.root!=i || value.set==UINT32_MAX || value.binding==UINT32_MAX)continue;
        ShaderBinding b{value.set,value.binding,value.storage,false,false};
        for(auto p:sampled)if(ids[p].root==i)b.sampled=true;
        for(auto p:dref)if(ids[p].root==i)b.dref=true;
        out.bindings.push_back(b);
    }
    for(auto p:sampled) {
        auto r=ids[p].root;
        if(!r || ids[r].set==UINT32_MAX || ids[r].binding==UINT32_MAX)++out.unresolvedSamples;
    }
    out.replaySafe &= out.isolationSafe && !(out.earlyTests && (discard || sampleMask));out.valid=true;inspect_sampler_accesses(out);
    out.discards=discard;out.dxvkAlphaTest=find_dxvk_alpha_test(out.words.data(),out.words.size());
    return out;
}
AlphaTestState dxvk_alpha_test_state(const Shader* fs,const VkSpecializationInfo* spec) {
    AlphaTestState out;
    if(!fs)return out;
    const auto& t=fs->dxvkAlphaTest;
    if(!t.found){out.source="no DXVK alpha-test pattern";return out;}
    uint32_t use=t.useDefault,word=t.wordDefault;
    if(spec && spec->pMapEntries && spec->pData)for(uint32_t i=0;i<spec->mapEntryCount;++i) {
        const auto& e=spec->pMapEntries[i];
        if(e.size!=4 || e.offset>spec->dataSize || spec->dataSize-e.offset<4)continue;
        uint32_t v;std::memcpy(&v,static_cast<const char*>(spec->pData)+e.offset,4);
        if(e.constantID==t.useSpecId)use=v;
        if(e.constantID==t.wordSpecId)word=v;
    }
    if((use!=0)!=t.specWhenNonZero){out.source="fast-link (DXVK spec UBO)";return out;}
    out.compare=uint8_t((word>>t.offset)&((1u<<t.bits)-1));
    out.state=out.compare==VK_COMPARE_OP_ALWAYS?0:1;out.source="specialization";
    return out;
}
// Structural match, no interpretation: the selector of an OpSwitch with at least 6 cases is
// OpBitFieldUExtract(OpSelect(cond, specW, other) or OpSelect(cond, other, specW), constant offset, constant count), where
// cond compares spec constant U with the constant 0 (OpINotEqual or OpIEqual, or either folded into OpSpecConstantOp). Found in DXVK 2.x D3D9 shaders (WoW, 2026-09-27:
// U = 20, W = 1, bits 21..23, cases 0..6 + default, demote when the comparison fails).
DxvkAlphaTest find_dxvk_alpha_test(const uint32_t* words,size_t n) {
    DxvkAlphaTest none;
    if(!words || n<5 || words[0]!=0x07230203 || words[3]==0 || words[3]>262144)return none;
    const uint32_t bound=words[3];
    struct Def {uint32_t op=0,a=0,b=0,c=0;};
    std::vector<Def> defs(bound);
    std::vector<uint32_t> specId(bound,UINT32_MAX),value(bound,0);std::vector<uint8_t> isConst(bound,0),isSpec(bound,0);
    std::vector<std::pair<uint32_t,uint32_t>> switches;   // selector, case count
    // For the fast-link side (a uniform load through constant indices): loads, access chains, variables, types, decorations.
    std::vector<uint32_t> loadOf(bound,0),pointee(bound,0),varType(bound,0),arrayElem(bound,0),arrayStride(bound,0),dset(bound,UINT32_MAX),dbinding(bound,UINT32_MAX);
    std::vector<uint8_t> uniformVar(bound,0);
    std::vector<std::vector<uint32_t>> chain(bound),members(bound);
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> memberOffset;
    for(size_t i=5;i<n;) {
        const uint32_t count=words[i]>>16,op=words[i]&65535;
        if(!count || count>n-i)return none;
        const auto* w=words+i;
        auto id=[&](unsigned k){return k<count && w[k]<bound;};
        if(op==71 && count>=4 && w[2]==1 && id(1))specId[w[1]]=w[3];                                   // OpDecorate SpecId
        else if(op==43 && count>=4 && id(2)){isConst[w[2]]=1;value[w[2]]=w[3];}                         // OpConstant
        else if(op==50 && count>=4 && id(2)){isSpec[w[2]]=1;value[w[2]]=w[3];}                          // OpSpecConstant
        else if((op==169 || op==203) && count>=6 && id(2))defs[w[2]]={op,w[3],w[4],w[5]};              // OpSelect, OpBitFieldUExtract
        else if((op==170 || op==171) && count>=5 && id(2))defs[w[2]]={op,w[3],w[4],0};                 // OpIEqual, OpINotEqual
        else if(op==52 && count>=6 && (w[3]==170 || w[3]==171) && id(2))defs[w[2]]={w[3],w[4],w[5],0};   // the same, folded (OpSpecConstantOp)
        else if(op==251 && count>=3)switches.emplace_back(w[1],(count-3)/2);                            // OpSwitch
        else if(op==61 && count>=4 && id(2) && id(3))loadOf[w[2]]=w[3];                                  // OpLoad
        else if((op==65 || op==66) && count>=4 && id(2)){chain[w[2]].assign(w+3,w+count);}              // OpAccessChain: base, indices
        else if(op==59 && count>=4 && id(2) && id(1)){varType[w[2]]=w[1];uniformVar[w[2]]=w[3]==2;}      // OpVariable (Uniform)
        else if(op==32 && count>=4 && id(1))pointee[w[1]]=w[3];                                          // OpTypePointer
        else if(op==28 && count>=4 && id(1))arrayElem[w[1]]=w[2];                                        // OpTypeArray
        else if(op==30 && count>=2 && id(1))members[w[1]].assign(w+2,w+count);                           // OpTypeStruct
        else if(op==71 && count>=4 && id(1) && w[2]==34)dset[w[1]]=w[3];                                 // DescriptorSet
        else if(op==71 && count>=4 && id(1) && w[2]==33)dbinding[w[1]]=w[3];                             // Binding
        else if(op==71 && count>=4 && id(1) && w[2]==6)arrayStride[w[1]]=w[3];                           // ArrayStride
        else if(op==72 && count>=5 && w[3]==35)memberOffset[{w[1],w[2]}]=w[4];                            // OpMemberDecorate Offset
        i+=count;
    }
    auto spec=[&](uint32_t x){return x<bound && isSpec[x] && specId[x]!=UINT32_MAX;};
    auto zero=[&](uint32_t x){return x<bound && isConst[x] && value[x]==0;};
    for(auto [selector,cases]:switches) {
        if(cases<6 || selector>=bound || defs[selector].op!=203)continue;
        const auto& extract=defs[selector];
        if(extract.a>=bound || extract.b>=bound || extract.c>=bound || !isConst[extract.b] || !isConst[extract.c])continue;
        const auto& select=defs[extract.a];
        if(select.op!=169 || select.a>=bound)continue;
        const auto& cond=defs[select.a];
        if(cond.op!=170 && cond.op!=171)continue;
        const uint32_t use=spec(cond.a) && zero(cond.b)?cond.a:spec(cond.b) && zero(cond.a)?cond.b:0;
        if(!use)continue;
        const bool specFirst=spec(select.b),specSecond=spec(select.c);
        if(specFirst==specSecond)continue;   // exactly one side is the spec word
        DxvkAlphaTest out;out.found=true;
        out.useSpecId=specId[use];out.useDefault=value[use];
        const uint32_t word=specFirst?select.b:select.c;out.wordSpecId=specId[word];out.wordDefault=value[word];
        out.offset=value[extract.b];out.bits=value[extract.c];
        // The select's first operand is taken when cond is true: spec path when (use != 0) for INotEqual, (use == 0) for IEqual.
        out.specWhenNonZero=(cond.op==171)==specFirst;
        if(out.bits==0 || out.bits>8 || out.offset+out.bits>32)continue;
        // The other operand: a uniform word loaded through constant indices (spec_state[k] or spec_state.dwords[k]) -> set,
        // binding and byte offset, walking the struct / array types with their Offset / ArrayStride decorations.
        const uint32_t other=specFirst?select.c:select.b;
        if(other<bound && loadOf[other] && loadOf[other]<bound) {
            const auto& ch=chain[loadOf[other]];
            if(!ch.empty() && ch[0]<bound && uniformVar[ch[0]] && dset[ch[0]]!=UINT32_MAX && dbinding[ch[0]]!=UINT32_MAX && varType[ch[0]]<bound) {
                uint32_t type=pointee[varType[ch[0]]],offset=0;bool ok=type!=0;
                for(size_t k=1;ok && k<ch.size();++k) {
                    if(ch[k]>=bound || !isConst[ch[k]] || type>=bound){ok=false;break;}
                    const uint32_t index=value[ch[k]];
                    if(!members[type].empty()) {
                        auto found=memberOffset.find({type,index});
                        if(index>=members[type].size() || found==memberOffset.end()){ok=false;break;}
                        offset+=found->second;type=members[type][index];
                    } else if(arrayElem[type]) {
                        if(!arrayStride[type]){ok=false;break;}
                        offset+=index*arrayStride[type];type=arrayElem[type];
                    } else ok=false;
                }
                if(ok){out.uboKnown=true;out.uboSet=dset[ch[0]];out.uboBinding=dbinding[ch[0]];out.uboOffset=offset;}
            }
        }
        return out;
    }
    return none;
}
}
