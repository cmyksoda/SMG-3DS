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
int main() {
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
    check(!bcsv.load({bad.data(),bad.size()},error),"misaligned numeric field rejected");
    bad=bytes;bad[26]=32;
    check(!bcsv.load({bad.data(),bad.size()},error),"invalid shift rejected");
    bad=bytes;bad.back()='x';
    check(!bcsv.load({bad.data(),bad.size()},error),"unterminated string table rejected");
    for(std::size_t i=0;i<bytes.size();++i)
        check(!bcsv.load({bytes.data(),i},error),"truncated table rejected");
    std::printf("%u original JMap/BCSV checks passed\n",checks);
}
