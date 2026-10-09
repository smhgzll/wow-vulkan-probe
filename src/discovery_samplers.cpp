#include "discovery_internal.hpp"

namespace depth_probe::discovery {
// A small, fail-closed evaluator for sampler indices. It accepts constants or
// a 32-bit push word with constant shift/mask/extract, never arbitrary shader
// execution, UBO contents, specialization guesses, or invocation-dependent IDs.
void inspect_sampler_accesses(Shader& shader) {
    const auto& words=shader.words;
    struct Id {
        uint32_t kind=0,type=0,element=0,width=0,set=UINT32_MAX,binding=UINT32_MAX;
        uint32_t pushAddress=UINT32_MAX;
        SamplerIndex value,selector;
        std::vector<uint32_t> members;
        std::map<uint32_t,uint32_t> offsets;
    };
    std::vector<Id> ids(words[3]);
    auto constant=[&](uint32_t id,uint32_t& result) {
        if(id>=ids.size() || !ids[id].value.known || ids[id].value.fromPush)return false;
        result=ids[id].value.value;return true;
    };
    auto integer32=[&](uint32_t type){return type<ids.size() && ids[type].kind==21 && ids[type].width==32;};
    for(size_t i=5;i<words.size();) {
        const uint32_t count=words[i]>>16,op=words[i]&65535;const auto* w=words.data()+i;
        auto valid=[&](unsigned n){return n<count && w[n]<ids.size();};
        if(op==71 && count>=4 && valid(1)) {
            if(w[2]==33)ids[w[1]].binding=w[3];
            if(w[2]==34)ids[w[1]].set=w[3];
        }
        if(op==72 && count>=5 && valid(1) && w[3]==35)ids[w[1]].offsets[w[2]]=w[4];
        if(op==21 && count>=4 && valid(1)){ids[w[1]].kind=op;ids[w[1]].width=w[2];}
        if(op==26 && valid(1))ids[w[1]].kind=op;
        if(((op==28 && count>=4) || (op==29 && count>=3) || (op==32 && count>=4)) && valid(1)) {
            auto& id=ids[w[1]];id.kind=op;id.element=w[op==32?3:2];
        }
        if(op==30 && valid(1)){ids[w[1]].kind=op;ids[w[1]].members.assign(w+2,w+count);}
        if(op==43 && count==4 && valid(2) && integer32(w[1]))ids[w[2]].value={true,false,w[3]};
        if(op==59 && count>=4 && valid(1) && valid(2)) {
            auto& id=ids[w[2]];id.type=w[1];id.element=ids[w[1]].element;
            if(w[3]==9)id.pushAddress=0;
            if(id.element<ids.size() && ids[id.element].kind==26)id.selector={true,false,0};
        }
        if(op==65 && count>=5 && valid(2) && valid(3)) {
            auto& out=ids[w[2]];const auto& base=ids[w[3]];
            out.type=w[1];out.element=valid(1)?ids[w[1]].element:0;
            out.set=base.set;out.binding=base.binding;
            if(count==5 && base.element<ids.size()) {
                const auto& type=ids[base.element];
                if((type.kind==28 || type.kind==29) && type.element<ids.size() && ids[type.element].kind==26 && valid(4))out.selector=ids[w[4]].value;
                uint32_t member=0;
                if(base.pushAddress!=UINT32_MAX && type.kind==30 && constant(w[4],member) && member<type.members.size() && type.offsets.count(member)) {
                    const uint64_t offset=uint64_t(base.pushAddress)+type.offsets.at(member);
                    if(offset<maxPushWords*4 && integer32(type.members[member]))out.pushAddress=uint32_t(offset);
                }
            }
        }
        if(op==61 && count>=4 && valid(1) && valid(2) && valid(3)) {
            const auto& pointer=ids[w[3]];
            if(integer32(w[1]) && pointer.pushAddress!=UINT32_MAX && pointer.pushAddress%4==0)
                ids[w[2]].value={true,true,pointer.pushAddress};
            if(ids[w[1]].kind==26) {
                if(pointer.set==UINT32_MAX || pointer.binding==UINT32_MAX)shader.samplerAccessesComplete=false;
                else {
                    const auto same=[&](const SamplerAccess& a) {const auto& x=a.index;const auto& y=pointer.selector;return a.set==pointer.set && a.binding==pointer.binding && x.known==y.known && x.fromPush==y.fromPush && x.value==y.value && x.shift==y.shift && x.mask==y.mask;};
                    if(std::none_of(shader.samplers.begin(),shader.samplers.end(),same)) {
                        if(shader.samplers.size()>=32)shader.samplerAccessesComplete=false;
                        else shader.samplers.push_back({pointer.set,pointer.binding,pointer.selector});
                    }
                }
            }
        }
        if((op==83 || op==113 || op==124) && count==4 && valid(2) && valid(3)) {
            if(op==83){auto& out=ids[w[2]];out=ids[w[3]];}
            else if(integer32(w[1]))ids[w[2]].value=ids[w[3]].value;
        }
        if((op==194 || op==199 || op==203) && valid(2) && valid(3) && integer32(w[1])) {
            auto value=ids[w[3]].value;uint32_t amount=0,width=0;
            if(value.known && valid(4) && constant(w[4],amount)) {
                if(op==199 && count==5) {
                    if(value.fromPush)value.mask&=amount;else value.value&=amount;
                    ids[w[2]].value=value;
                } else if((op==194 && count==5 && amount<32) ||
                          (op==203 && count==6 && constant(w[5],width) && amount<=32 && width<=32-amount)) {
                    if(op==194)width=32-amount;
                    const uint32_t mask=width==32?UINT32_MAX:width?(uint32_t(1)<<width)-1:0;
                    if(!mask || (value.fromPush && value.shift+amount>=32))value={true,false,0};
                    else if(value.fromPush){value.shift+=amount;value.mask=(value.mask>>amount)&mask;}
                    else value.value=(value.value>>amount)&mask;
                    ids[w[2]].value=value;
                }
            }
        }
        i+=count;
    }
}
bool unresolved_update_after_bind(const ResourceUse& r) {
    return r.updateAfterBind && !(r.samplerIndexed && r.descriptor.type==VK_DESCRIPTOR_TYPE_SAMPLER && r.descriptor.known && r.descriptor.sampler);
}
bool coverage_resources_supported(const SubmittedDraw& d) {
    // An unreflected sampler index prevents material identity, not an occlusion
    // query: the original shader and query use the same descriptor table at GPU
    // execution. Other missing resources and UAB images remain unsupported.
    return (!d.descriptorsIncomplete || d.descriptorIssues==SamplerIndexUnknown) &&
        std::none_of(d.resources.begin(),d.resources.end(),unresolved_update_after_bind);
}
void resolve_sampler_resources(const Device& s,SubmittedDraw& out,uint32_t number,const Set& set) {
    const auto p=out.draw.pipeline;if(!p || !set.layout)return;
    auto fail=[&](uint32_t issue=SamplerUnavailable) {out.descriptorsIncomplete=true;out.descriptorIssues|=issue;};
    std::set<std::pair<uint32_t,uint32_t>> used;
    const std::shared_ptr<Shader> shaders[]{p->vs,p->fs};
    for(size_t stage=0;stage<2;++stage)if(auto shader=shaders[stage]) {
        if(!shader->samplerAccessesComplete)fail(SamplerIndexUnknown);
        for(const auto& access:shader->samplers)if(access.set==number) {
            auto binding=set.layout->bindings.find(access.binding);
            if(binding==set.layout->bindings.end() || binding->second.type!=VK_DESCRIPTOR_TYPE_SAMPLER){fail();continue;}
            if(!access.index.known){fail(SamplerIndexUnknown);continue;}
            const auto& index=access.index;uint32_t element=index.value;
            if(index.fromPush) {
                const auto& push=out.draw.push[stage];const uint32_t word=index.value/4;
                if(word>=maxPushWords || !p->layout || push.signature!=p->layout->pushSignature || !(push.known&(uint64_t(1)<<word))){fail(SamplerIndexUnknown);continue;}
                element=(push.words[word]>>index.shift)&index.mask;
            }
            if(element>=binding->second.count){fail();continue;}
            if(!used.emplace(access.binding,element).second)continue;
            const auto found=set.descriptors.find({access.binding,element});
            if(found==set.descriptors.end() || !found->second.known || !found->second.sampler){fail();continue;}
            const auto live=find_ptr(s.samplers,found->second.sampler->handle);
            if(live!=found->second.sampler){fail();continue;}
            if(out.resources.size()>=maxResourceUses){out.descriptorsIncomplete=true;out.descriptorIssues|=ResourceLimit;continue;}
            ResourceUse r;r.set=number;r.binding=access.binding;r.element=element;r.descriptor=found->second;
            r.updateAfterBind=binding->second.flags&VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;r.samplerIndexed=true;
            out.resources.push_back(std::move(r));
        }
    }
}
// If an observed sampler slot changes around QueueSubmit, do not attribute the
// old CPU snapshot to the query. Updating unrelated, unused heap slots is fine.
void revalidate_sampler_resources(const Device& s,SubmittedDraw& out) {
    for(const auto& r:out.resources)if(r.samplerIndexed) {
        const auto& bound=out.draw.sets[r.set];const auto set=bound.pushed?bound.pushed:find_ptr(s.sets,bound.handle);
        bool same=set && (bound.pushed || set->generation==bound.generation);
        if(same) {
            const auto it=set->descriptors.find({r.binding,r.element});
            same=it!=set->descriptors.end() && it->second.sampler==r.descriptor.sampler;
        }
        if(!same){out.descriptorsIncomplete=true;out.descriptorIssues|=SamplerUnavailable;}
    }
}
}
