#include <smg/Bcsv.hpp>
#include <JSystem/JGadget/hashcode.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks=0;
void check(bool passed,const char* reason) {
    ++checks;
    if (!passed) { std::fprintf(stderr,"FAIL: %s\n",reason); std::exit(1); }
}
void put32(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value) {
    bytes[offset]=value>>24; bytes[offset+1]=value>>16; bytes[offset+2]=value>>8; bytes[offset+3]=value;
}
void field(std::vector<std::uint8_t>& bytes,unsigned i,const char* name,unsigned offset,unsigned type,unsigned mask=0xffffffff,unsigned shift=0) {
    const auto at=16+i*12;
    put32(bytes,at,JGadget::getHashCode(name)); put32(bytes,at+4,mask);
    bytes[at+8]=offset>>8;bytes[at+9]=offset;bytes[at+10]=shift;bytes[at+11]=type;
}
}
int runBcsvTests() {
    check(JGadget::getHashCode("name")==3373707u,"original field hashing");
    constexpr unsigned dataOffset=100;
    std::vector<std::uint8_t> bytes(dataOffset+48+6,0);
    put32(bytes,0,1); put32(bytes,4,7); put32(bytes,8,dataOffset); put32(bytes,12,48);
    field(bytes,0,"id",0,0); field(bytes,1,"pos_x",4,2); field(bytes,2,"name",8,6);
    field(bytes,3,"short",12,4,0xffff); field(bytes,4,"byte",14,5,0xff);
    field(bytes,5,"inline",16,1); field(bytes,6,"bits",0,0,0xf0,4);
    put32(bytes,dataOffset,0xfffffff0); put32(bytes,dataOffset+4,0x3fc00000); put32(bytes,dataOffset+8,0);
    bytes[dataOffset+12]=0xff;bytes[dataOffset+13]=0xfe;bytes[dataOffset+14]=0xfd;
    std::memcpy(bytes.data()+dataOffset+16,"Mario",6);std::memcpy(bytes.data()+dataOffset+48,"Luigi",6);
    smg::Bcsv bcsv;std::string error;
    check(bcsv.load({bytes.data(),bytes.size()},error),"big-endian BCSV converted");
    const auto& info=bcsv.info();
    check(info.getNumEntries()==1 && info.getNumFields()==7,"original JMapInfo counts");
    s32 integer=0;u32 masked=0;f32 real=0;const char* string=nullptr;
    check(info.getValue(0,"id",&integer) && integer==-16,"original signed integer lookup");
    check(info.getValue(0,"pos_x",&real) && std::fabs(real-1.5f)<0.0001f,"original float lookup");
    check(info.getValue(0,"short",&integer) && integer==-2,"original signed short lookup");
    check(info.getValue(0,"byte",&integer) && integer==-3,"original signed byte lookup");
    check(info.getValue(0,"bits",&masked) && masked==15,"overlapping bitfield decoded once");
    check(info.getValue(0,"name",&string) && std::strcmp(string,"Luigi")==0,"original string-table lookup");
    check(info.getValue(0,"inline",&string) && std::strcmp(string,"Mario")==0,"original inline string lookup");
    check(!info.getValue(0,"absent",&integer),"missing field remains absent");
    check(info.findElementBinary("name","Luigi").isValid(),"original binary search");
    check(MR::findJMapInfoElementNoCase(&info,"name","lUiGi",0).isValid(),"original case-insensitive lookup");
    auto bad=bytes;put32(bad,dataOffset+8,20);
    check(!bcsv.load({bad.data(),bad.size()},error) && !bcsv.info().dataExists(),"invalid string offset detaches old data");
    bad=bytes;put32(bad,8,0xfffffffc);
    check(!bcsv.load({bad.data(),bad.size()},error),"invalid record offset");
    bad=bytes;bad[24]=0;bad[25]=1;
    check(!bcsv.load({bad.data(),bad.size()},error),"incompatible numeric overlap rejected");
    bad=bytes;bad[26]=32;
    check(!bcsv.load({bad.data(),bad.size()},error),"invalid shift rejected");
    bad=bytes;bad.back()='x';
    check(!bcsv.load({bad.data(),bad.size()},error),"unterminated string table rejected");
    for(std::size_t i=0;i<bytes.size();++i)
        check(!bcsv.load({bytes.data(),i},error),"truncated table rejected");
    std::vector<std::uint8_t> packed(56+18+6,0);
    put32(packed,0,2); put32(packed,4,3); put32(packed,8,56); put32(packed,12,9);
    field(packed,0,"flag",0,5,255); field(packed,1,"name",1,6); field(packed,2,"speed",5,2);
    packed[56]=4; packed[65]=7;
    put32(packed,57,0); put32(packed,66,2);
    put32(packed,61,0x3fc00000); put32(packed,70,0xc0200000);
    std::memcpy(packed.data()+74,"A\0BCD",6);
    check(bcsv.load({packed.data(),packed.size()},error),"packed fields and odd record stride repacked");
    check(info.getValue(0,"flag",&integer) && integer==4,"packed first row byte");
    check(info.getValue(1,"flag",&integer) && integer==7,"packed second row byte");
    check(info.getValue(0,"name",&string) && std::strcmp(string,"A")==0,"repacked string base");
    check(info.getValue(1,"name",&string) && std::strcmp(string,"BCD")==0,"repacked second row string");
    check(info.getValue(0,"speed",&real) && real==1.5f,"unaligned float decoded");
    check(info.getValue(1,"speed",&real) && real==-2.5f,"odd-stride second row float decoded");
    // Attached as the game attaches it, a table ARM cannot read in place comes back as a repacked native copy.
    std::vector<std::uint32_t> packedDisc((packed.size()+3)/4);
    std::memcpy(packedDisc.data(),packed.data(),packed.size());
    JMapInfo packedAttached;
    check(packedAttached.attach(packedDisc.data()) && packedAttached.mData!=reinterpret_cast<const JMapData*>(packedDisc.data()),"odd-stride table attaches as a copy");
    check(packedAttached.getValue(1,"flag",&integer) && integer==7 && packedAttached.getValue(1,"speed",&real) && real==-2.5f,"repacked copy reads as the port's loader");
    check(packedAttached.getValue(1,"name",&string) && std::strcmp(string,"BCD")==0,"repacked copy keeps its strings");
    std::free(const_cast<JMapData*>(packedAttached.mData));
    // The game's own loading hands JMapInfo the disc's big-endian table; attaching turns it native in place.
    std::vector<std::uint32_t> disc((bytes.size()+3)/4);
    std::memcpy(disc.data(),bytes.data(),bytes.size());
    JMapInfo attached;
    check(attached.attach(disc.data()) && attached.getNumEntries()==1 && attached.getNumFields()==7,"big-endian table attaches natively");
    s32 value=0; f32 speed=0; const char* text=nullptr;
    check(attached.getValue(0,"id",&value) && value==-16,"attached signed integer");
    u32 bits=0;
    check(attached.getValue(0,"bits",&bits) && bits==15,"attached word shared by two fields turned once");
    check(attached.getValue(0,"pos_x",&speed) && std::fabs(speed-1.5f)<0.0001f,"attached float");
    check(attached.getValue(0,"short",&value) && value==-2 && attached.getValue(0,"byte",&value) && value==-3,"attached short and byte");
    check(attached.getValue(0,"name",&text) && std::strcmp(text,"Luigi")==0,"attached string table");
    check(attached.getValue(0,"inline",&text) && std::strcmp(text,"Mario")==0,"attached inline string");
    JMapInfo again;
    check(again.attach(disc.data()) && again.getValue(0,"id",&value) && value==-16,"attaching a turned table again changes nothing");
    std::printf("%u original JMap/BCSV checks passed\n",checks);
    return 0;
}
#ifndef SMG_TEST_RUNNER
int main() { return runBcsvTests(); }
#endif
