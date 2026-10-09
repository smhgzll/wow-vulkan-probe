#include "water_reflections.hpp"

namespace depth_probe::discovery {
namespace {
using Words=std::vector<uint32_t>;
void emit(Words& out,uint32_t op,std::initializer_list<uint32_t> args) {
    out.push_back((uint32_t(args.size()+1)<<16)|op);out.insert(out.end(),args);
}
struct Output {uint32_t variable=0,structure=0,member=UINT32_MAX,type=0;};
}
std::vector<uint32_t> reflect_vertex_spirv(const Words& code,const std::string& name,std::string& reason) {
    auto fail=[&](const char* why)->Words{reason=why;return {};};
    if(code.size()<5 || code[0]!=0x07230203 || code[3]>262144)return fail("invalid SPIR-V");
    std::vector<Words> instructions;std::map<uint32_t,Words> defs;
    std::map<uint32_t,uint32_t> builtins;
    std::map<std::pair<uint32_t,uint32_t>,uint32_t> members;
    uint32_t entry=0,entries=0;bool clipCapability=false;
    for(size_t i=5;i<code.size();) {
        const uint32_t count=code[i]>>16,op=code[i]&65535;
        if(!count || count>code.size()-i)return fail("malformed instruction");
        Words w(code.begin()+i,code.begin()+i+count);instructions.push_back(w);
        if(op==15) {
            ++entries;if(count<4 || w[1]!=0)return fail("vertex entry required");
            const auto text=reinterpret_cast<const char*>(w.data()+3);
            const auto length=strnlen(text,(count-3)*4);
            if(length==(count-3)*4 || std::string(text,length)!=name)return fail("entry mismatch");
            entry=w[2];
        }
        if(op==17 && count==2 && w[1]==32)clipCapability=true;
        if(op==71 && count==4 && w[2]==34 && w[3]>=3)return fail("descriptor set 3 already used");
        if(op==71 && count==4 && w[2]==11)builtins[w[1]]=w[3];
        if(op==72 && count==5 && w[3]==11)members[{w[1],w[2]}]=w[4];
        if(op>=19 && op<=39 && count>=2)defs[w[1]]=w;
        if((op==43 || op==59) && count>=4)defs[w[2]]=w;
        i+=count;
    }
    if(entries!=1 || !entry || !clipCapability)return fail("single vertex entry with enabled clip distance required");
    Output position,clip;uint32_t cullCount=0;
    for(const auto& [id,w]:defs)if((w[0]&65535)==59 && w[3]==3) {
        auto pointer=defs.find(w[1]);if(pointer==defs.end() || pointer->second.size()!=4)continue;
        const auto type=pointer->second[3];
        auto record=[&](uint32_t builtin,uint32_t member,uint32_t valueType) {
            Output value{id,member==UINT32_MAX?0:type,member,valueType};
            if(builtin==0)position=value;
            if(builtin==3)clip=value;
            if(builtin==4 && defs.count(valueType) && defs.at(valueType).size()==4) {
                const auto len=defs.at(valueType)[3];if(defs.count(len) && defs.at(len).size()==4)cullCount=defs.at(len)[3];else cullCount=8;
            }
        };
        if(builtins.count(id))record(builtins[id],UINT32_MAX,type);
        auto structure=defs.find(type);
        if(structure!=defs.end() && (structure->second[0]&65535)==30)
            for(const auto& [key,builtin]:members)if(key.first==type && key.second+2<structure->second.size())record(builtin,key.second,structure->second[key.second+2]);
    }
    if(!position.variable || !clip.variable || !defs.count(clip.type))return fail("position/clip output unsupported");
    const auto array=defs.at(clip.type);
    if((array[0]&65535)!=28 || array.size()!=4 || !defs.count(array[3]))return fail("static clip array required");
    const auto length=defs.at(array[3]);
    if((length[0]&65535)!=43 || length.size()!=4 || !length[3] || length[3]+cullCount>=8)return fail("clip distance limit");
    if(!defs.count(position.type))return fail("position type missing");
    const auto vector=defs.at(position.type);
    if((vector[0]&65535)!=23 || vector.size()!=4 || vector[3]!=4 || array[2]!=vector[2])return fail("float4 position required");
    const uint32_t f32=vector[2],v4=position.type;
    if(!defs.count(f32) || defs.at(f32).size()!=3 || (defs.at(f32)[0]&65535)!=22 || defs.at(f32)[2]!=32)return fail("float32 required");
    // Growing the output array is valid for scalar element accesses. Reject
    // whole-array/block copies and intermediate array pointers whose types
    // would otherwise retain the old length.
    for(const auto& w:instructions) {
        const auto op=w[0]&65535;
        if((op==61 && w.size()>3 && w[3]==clip.variable) || (op==62 && w.size()>1 && w[1]==clip.variable) ||
           ((op==63 || op==64) && w.size()>2 && (w[1]==clip.variable || w[2]==clip.variable)))return fail("whole clip output access unsupported");
        if((op==65 || op==66) && w.size()>3 && w[3]==clip.variable && defs.count(w[1]) && defs.at(w[1]).size()==4 && defs.at(w[1])[3]==clip.type)return fail("intermediate clip array pointer unsupported");
    }
    uint32_t bound=code[3];auto fresh=[&]{return bound++;};
    const uint32_t newLength=fresh(),newArray=fresh(),newArrayPointer=fresh();
    Words globals,annotations;
    auto type=[&](uint32_t op,std::initializer_list<uint32_t> args) {
        for(const auto& [id,w]:defs)if((w[0]&65535)==op && w.size()==args.size()+2 && std::equal(args.begin(),args.end(),w.begin()+2))return id;
        uint32_t id=fresh();Words w{(uint32_t(args.size()+2)<<16)|op,id};w.insert(w.end(),args);defs[id]=w;globals.insert(globals.end(),w.begin(),w.end());return id;
    };
    auto constant=[&](uint32_t t,uint32_t bits) {
        for(const auto& [id,w]:defs)if((w[0]&65535)==43 && w.size()==4 && w[1]==t && w[3]==bits)return id;
        uint32_t id=fresh();emit(globals,43,{t,id,bits});defs[id]={4u<<16|43u,t,id,bits};return id;
    };
    const auto i32=type(21,{32,1}),v2i=type(23,{i32,2}),boolean=type(20,{}),m4=type(24,{v4,4});
    const auto image=type(25,{f32,1,0,0,0,1,0}),sampled=type(27,{image}),samplerPointer=type(32,{0,sampled});
    const auto positionPointer=type(32,{3,v4}),clipPointer=type(32,{3,f32});
    const auto camera=fresh();emit(globals,59,{samplerPointer,camera,0});
    emit(annotations,71,{camera,34,3});emit(annotations,71,{camera,33,0});
    const auto zero=constant(i32,0),zeroFloat=constant(f32,0),negative=constant(f32,0xbf800000);
    const auto clipIndex=constant(i32,length[3]);
    const auto positionMember=position.member==UINT32_MAX?0:constant(i32,position.member);
    const auto clipMember=clip.member==UINT32_MAX?0:constant(i32,clip.member);
    std::array<uint32_t,6> coords{};uint32_t columns[]{0,1,2,3,8,18};   // 18.x: reference plane valid (the mirror needs it)
    for(unsigned i=0;i<6;++i){auto x=constant(i32,columns[i]);coords[i]=fresh();emit(globals,44,{v2i,coords[i],x,zero});}
    auto transform=[&](Words& out) {
        uint32_t pos=position.variable;
        if(position.member!=UINT32_MAX){pos=fresh();emit(out,65,{positionPointer,pos,position.variable,positionMember});}
        const auto original=fresh();emit(out,61,{v4,original,pos});
        const auto combined=fresh(),im=fresh();emit(out,61,{sampled,combined,camera});emit(out,100,{image,im,combined});
        std::array<uint32_t,6> texels{};
        for(unsigned i=0;i<6;++i){texels[i]=fresh();emit(out,95,{v4,texels[i],im,coords[i],2,zero});}
        const auto matrix=fresh(),reflected=fresh(),distance=fresh(),validFloat=fresh(),valid=fresh(),clipped=fresh();
        emit(out,80,{m4,matrix,texels[0],texels[1],texels[2],texels[3]});
        emit(out,145,{v4,reflected,matrix,original});emit(out,62,{pos,reflected});
        emit(out,148,{f32,distance,texels[4],original});
        emit(out,81,{f32,validFloat,texels[5],0});emit(out,186,{boolean,valid,validFloat,zeroFloat});
        emit(out,169,{f32,clipped,valid,distance,negative});
        const auto dst=fresh();
        if(clip.member==UINT32_MAX)emit(out,65,{clipPointer,dst,clip.variable,clipIndex});
        else emit(out,65,{clipPointer,dst,clip.variable,clipMember,clipIndex});
        emit(out,62,{dst,clipped});
    };
    Words result(code.begin(),code.begin()+5);bool decorated=false,declared=false;uint32_t function=0,returns=0;
    for(auto w:instructions) {
        const auto op=w[0]&65535;
        if(!decorated && op>=19 && op<=39){result.insert(result.end(),annotations.begin(),annotations.end());decorated=true;}
        if(!declared && op==54){result.insert(result.end(),globals.begin(),globals.end());declared=true;}
        if(op==15 && code[1]>=0x00010400){w.push_back(camera);w[0]=(uint32_t(w.size())<<16)|15;}
        if(op==30 && w[1]==clip.structure)w[clip.member+2]=newArray;
        if(op==59 && w[2]==clip.variable && clip.member==UINT32_MAX)w[1]=newArrayPointer;
        if(op==54)function=w[2];
        if(op==253 && function==entry){transform(result);++returns;}
        result.insert(result.end(),w.begin(),w.end());
        if(op==28 && w[1]==clip.type) {
            emit(result,43,{length[1],newLength,length[3]+1});
            emit(result,28,{newArray,f32,newLength});emit(result,32,{newArrayPointer,3,newArray});
        }
        if(op==56)function=0;
    }
    if(function || !returns || !decorated || !declared)return fail("entry return missing");
    result[3]=bound;reason.clear();return result;
}
}

namespace depth_probe::discovery {
// Keep original sampling, alpha test/discard/demote and specialization. Only the
// final color output becomes view-space position plus original alpha coverage; a second output (location 1) keeps the original color.
std::vector<uint32_t> water_fragment_spirv(const Words& code,const std::string& name,std::string& reason) {
    auto fail=[&](const char* why)->Words{reason=why;return {};};
    if(code.size()<5 || code[0]!=0x07230203 || code[3]>262144)return fail("invalid fragment SPIR-V");
    std::vector<Words> instructions;std::map<uint32_t,Words> defs;std::map<uint32_t,uint32_t> locations,builtins;
    uint32_t entry=0,entries=0;
    for(size_t i=5;i<code.size();) {
        const auto count=code[i]>>16,op=code[i]&65535;if(!count || count>code.size()-i)return fail("malformed fragment instruction");
        Words w(code.begin()+i,code.begin()+i+count);instructions.push_back(w);
        if(op==15) {
            ++entries;if(count<4 || w[1]!=4)return fail("fragment entry required");
            const auto text=reinterpret_cast<const char*>(w.data()+3);const auto length=strnlen(text,(count-3)*4);
            if(length==(count-3)*4 || std::string(text,length)!=name)return fail("fragment entry mismatch");
            entry=w[2];
        }
        if(op==71 && count==4 && w[2]==34 && w[3]>=3)return fail("fragment set 3 already used");
        if(op==71 && count==4 && w[2]==30)locations[w[1]]=w[3];
        if(op==71 && count==4 && w[2]==11)builtins[w[1]]=w[3];
        if(op>=19 && op<=39 && count>=2)defs[w[1]]=w;
        if((op==43 || op==59) && count>=4)defs[w[2]]=w;
        i+=count;
    }
    if(entries!=1 || !entry)return fail("single fragment entry required");
    uint32_t color=0,fragCoord=0,v4=0;
    for(const auto& [id,w]:defs)if((w[0]&65535)==59) {
        if(w[3]==3) {
            if(!locations.count(id) || locations[id]!=0 || color)return fail("single float4 color output required");
            if(!defs.count(w[1]) || defs.at(w[1]).size()!=4)return fail("fragment output pointer");
            color=id;v4=defs.at(w[1])[3];
        } else if(w[3]==1 && builtins.count(id) && builtins[id]==15)fragCoord=id;
    }
    if(!color || !defs.count(v4) || (defs.at(v4)[0]&65535)!=23 || defs.at(v4).size()!=4 || defs.at(v4)[3]!=4)return fail("fragment float4 color required");
    const auto f32=defs.at(v4)[2];if(!defs.count(f32) || (defs.at(f32)[0]&65535)!=22 || defs.at(f32)[2]!=32)return fail("fragment float32 required");
    uint32_t bound=code[3];auto fresh=[&]{return bound++;};Words globals,annotations;
    auto type=[&](uint32_t op,std::initializer_list<uint32_t> args) {
        for(const auto& [id,w]:defs)if((w[0]&65535)==op && w.size()==args.size()+2 && std::equal(args.begin(),args.end(),w.begin()+2))return id;
        uint32_t id=fresh();Words w{(uint32_t(args.size()+2)<<16)|op,id};w.insert(w.end(),args);defs[id]=w;globals.insert(globals.end(),w.begin(),w.end());return id;
    };
    auto constant=[&](uint32_t t,uint32_t bits) {
        for(const auto& [id,w]:defs)if((w[0]&65535)==43 && w.size()==4 && w[1]==t && w[3]==bits)return id;
        uint32_t id=fresh();emit(globals,43,{t,id,bits});defs[id]={4u<<16|43u,t,id,bits};return id;
    };
    auto i32=type(21,{32,1}),v2i=type(23,{i32,2}),m4=type(24,{v4,4});
    auto imType=type(25,{f32,1,0,0,0,1,0}),sampled=type(27,{imType}),samplerPointer=type(32,{0,sampled});
    const auto camera=fresh();emit(globals,59,{samplerPointer,camera,0});emit(annotations,71,{camera,34,3});emit(annotations,71,{camera,33,0});
    // Second output (location 1): the original color, untouched. The water replay records it to undo the game's blend later (seabed clarity).
    const auto outputVec4=type(32,{3,v4});
    const auto originalColor=fresh();emit(globals,59,{outputVec4,originalColor,3});emit(annotations,71,{originalColor,30,1});
    const bool addCoord=!fragCoord;
    if(addCoord){auto ptr=type(32,{1,v4});fragCoord=fresh();emit(globals,59,{ptr,fragCoord,1});emit(annotations,71,{fragCoord,11,15});}
    const auto zero=constant(i32,0),one=constant(f32,0x3f800000),two=constant(f32,0x40000000);
    uint32_t columns[]{4,5,6,7,11,12,13};std::array<uint32_t,7> coords{};
    for(unsigned i=0;i<7;++i){auto x=constant(i32,columns[i]);coords[i]=fresh();emit(globals,44,{v2i,coords[i],x,zero});}
    auto writeMask=[&](Words& out) {
        auto extract=[&](uint32_t vector,uint32_t component){auto value=fresh();emit(out,81,{f32,value,vector,component});return value;};
        auto binary=[&](uint32_t op,uint32_t a,uint32_t b){auto value=fresh();emit(out,op,{f32,value,a,b});return value;};
        const auto original=fresh(),fragment=fresh(),combined=fresh(),image=fresh();
        emit(out,61,{v4,original,color});emit(out,61,{v4,fragment,fragCoord});emit(out,61,{sampled,combined,camera});emit(out,100,{imType,image,combined});
        std::array<uint32_t,7> texels{};for(unsigned i=0;i<7;++i){texels[i]=fresh();emit(out,95,{v4,texels[i],image,coords[i],2,zero});}
        auto x=binary(131,binary(133,binary(136,binary(131,extract(fragment,0),extract(texels[4],0)),extract(texels[4],2)),two),one);
        auto y=binary(131,binary(133,binary(136,binary(131,extract(fragment,1),extract(texels[4],1)),extract(texels[4],3)),two),one);
        auto z=binary(136,binary(131,extract(fragment,2),extract(texels[5],0)),binary(131,extract(texels[5],1),extract(texels[5],0)));
        auto ndc=fresh(),matrix=fresh(),view=fresh();emit(out,80,{v4,ndc,x,y,z,one});emit(out,80,{m4,matrix,texels[0],texels[1],texels[2],texels[3]});emit(out,145,{v4,view,matrix,ndc});
        auto w=extract(view,3),vx=binary(136,extract(view,0),w),vy=binary(136,extract(view,1),w),vz=binary(136,extract(view,2),w);
        auto alpha=binary(133,extract(original,3),extract(texels[6],0));auto mask=fresh();emit(out,80,{v4,mask,vx,vy,vz,alpha});emit(out,62,{color,mask});emit(out,62,{originalColor,original});
    };
    Words result(code.begin(),code.begin()+5);bool decorated=false,declared=false;uint32_t function=0,returns=0;
    for(auto w:instructions) {
        auto op=w[0]&65535;
        if(!decorated && op>=19 && op<=39){result.insert(result.end(),annotations.begin(),annotations.end());decorated=true;}
        if(!declared && op==54){result.insert(result.end(),globals.begin(),globals.end());declared=true;}
        if(op==15){w.push_back(originalColor);if(addCoord)w.push_back(fragCoord);if(code[1]>=0x00010400)w.push_back(camera);w[0]=(uint32_t(w.size())<<16)|15;}
        if(op==54)function=w[2];
        if(op==253 && function==entry){writeMask(result);++returns;}
        result.insert(result.end(),w.begin(),w.end());if(op==56)function=0;
    }
    if(function || !returns || !decorated || !declared)return fail("fragment entry return missing");
    result[3]=bound;reason.clear();return result;
}
}
