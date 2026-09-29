#include <smg/Model.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks=0;
void check(bool pass,const char* message) { ++checks; if (!pass) {std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);} }
using Data=std::vector<std::uint8_t>;
void put16(Data& b,std::size_t at,unsigned v) {b[at]=v>>8;b[at+1]=v;}
void put32(Data& b,std::size_t at,std::uint32_t v) {put16(b,at,v>>16);put16(b,at+2,v);}
void putFloat(Data& b,std::size_t at,float v) {std::uint32_t bits;std::memcpy(&bits,&v,4);put32(b,at,bits);}
Data block(const char* name,unsigned size) {Data b(size,0);std::memcpy(b.data(),name,4);put32(b,4,size);return b;}
struct Fixture {Data data; std::size_t shape,vertex,material,texture;};
Fixture fixture(unsigned command=0x98) {
    auto inf=block("INF1",56);put32(inf,20,24);
    const unsigned commands[]{0x10,1,0x11,1,0x12,2,2,0};
    for (unsigned i=0;i<8;++i) put16(inf,24+i*4,commands[i]);
    auto vtx=block("VTX1",144);put32(vtx,8,64);put32(vtx,12,96);
    put32(vtx,64,9);put32(vtx,68,1);put32(vtx,72,4);put32(vtx,80,255);
    const float positions[]{-1,-1,0,1,-1,0,-1,1,0,1,1,0};
    for (unsigned i=0;i<12;++i) putFloat(vtx,96+i*4,positions[i]);
    auto drw=block("DRW1",24);put16(drw,8,1);put32(drw,12,20);put32(drw,16,22);
    auto jnt=block("JNT1",90);put16(jnt,8,1);put32(jnt,12,24);put32(jnt,16,88);
    for (unsigned i=0;i<3;++i) putFloat(jnt,28+i*4,1);
    auto shp=block("SHP1",131);put16(shp,8,1);
    put32(shp,12,44);put32(shp,16,84);put32(shp,24,86);put32(shp,28,102);
    put32(shp,32,104);put32(shp,36,115);put32(shp,40,123);
    put16(shp,46,1);put32(shp,86,9);put32(shp,90,3);put32(shp,94,255);
    const unsigned count=command==0x90?3:4;
    shp[104]=command;put16(shp,105,count);
    for (unsigned i=0;i<count;++i) put16(shp,107+i*2,i);
    if (command==0xa0 || command==0x80) {put16(shp,111,3);put16(shp,113,2);}
    put16(shp,117,1);put32(shp,123,3+count*2);
    auto mat=block("MAT3",475);put16(mat,8,1);put32(mat,12,132);put32(mat,16,464);
    put32(mat,28,466);put32(mat,32,470);put32(mat,88,474);
    put16(mat,132+72,65535);put16(mat,132+132,65535);put32(mat,466,2);put32(mat,470,0xffffffff);mat[474]=1;
    auto tex=block("TEX1",20);put32(tex,12,20);
    Fixture f;f.data.assign(32,0);std::memcpy(f.data.data(),"J3D2bmd3",8);put32(f.data,12,7);
    auto append=[&](const Data& b){auto offset=f.data.size();f.data.insert(f.data.end(),b.begin(),b.end());return offset;};
    append(inf);f.vertex=append(vtx);append(drw);append(jnt);f.shape=append(shp);f.material=append(mat);f.texture=append(tex);
    put32(f.data,8,f.data.size());return f;
}
}
int runModelTests() {
    smg::Model model;std::string error;
    auto f=fixture();
    check(smg::loadModel({f.data.data(),f.data.size()},model,4096,error),"minimal original-format model decoded");
    check(model.vertices.size()==6 && model.shapes.size()==1 && model.materials.size()==1,"strip triangulation counts");
    for (unsigned i=0;i<6;i+=3) {
        const auto *a=model.vertices[i].position,*b=model.vertices[i+1].position,*c=model.vertices[i+2].position;
        check((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])>0,"strip winding preserved on alternating triangles");
    }
    check(model.minimum[0]==-1 && model.maximum[1]==1,"decoded model bounds");
    for (unsigned primitive : {0x90u,0xa0u,0x80u}) {
        auto primitiveModel=fixture(primitive);
        check(smg::loadModel({primitiveModel.data.data(),primitiveModel.data.size()},model,4096,error),"triangle, fan and quad commands");
        check(model.vertices.size()==(primitive==0x90?3:6),"primitive triangle count");
    }
    for (std::size_t n=0;n<f.data.size();++n)
        check(!smg::loadModel({f.data.data(),n},model,4096,error),"truncated model rejected");
    check(!smg::loadModel({f.data.data(),f.data.size()},model,100,error),"geometry budget enforced");
    auto bad=f.data;put16(bad,f.shape+107,100);
    check(!smg::loadModel({bad.data(),bad.size()},model,4096,error),"out-of-range vertex index rejected");
    bad=f.data;put32(bad,f.shape+127,0xfffffff0);
    check(!smg::loadModel({bad.data(),bad.size()},model,4096,error),"overflowing display-list offset rejected");
    bad=f.data;put32(bad,f.material+12,0xffffffff);
    check(!smg::loadModel({bad.data(),bad.size()},model,4096,error),"material offset rejected before arithmetic");
    bad=f.data;putFloat(bad,f.vertex+96,NAN);
    check(!smg::loadModel({bad.data(),bad.size()},model,4096,error),"nonfinite positions rejected");
    bad=f.data;bad[f.shape+104]=0x61;
    check(!smg::loadModel({bad.data(),bad.size()},model,4096,error),"GX register writes cannot masquerade as geometry");

    Data cmpr(32,0);const unsigned endpoints[]{0xf800,0x07e0,0x001f,0xffff};
    for (unsigned i=0;i<4;++i) put16(cmpr,i*8,endpoints[i]);
    smg::ModelTexture texture;
    check(smg::decodeCmpr({cmpr.data(),cmpr.size()},8,8,texture,error),"CMPR macro tile decoded");
    check(texture.pixels[0]==0xff0000ff && texture.pixels[4]==0x00ff00ff &&
          texture.pixels[32]==0x0000ffff && texture.pixels[36]==0xffffffff,"CMPR sub-block order and RGB565 expansion");
    put16(cmpr,0,0xf800);put16(cmpr,2,0x07e0);cmpr[4]=0x1b;
    check(smg::decodeCmpr({cmpr.data(),cmpr.size()},8,8,texture,error),"CMPR four-color mode");
    check(texture.pixels[0]==0xff0000ff && texture.pixels[1]==0x00ff00ff &&
          texture.pixels[2]==0xaa5500ff && texture.pixels[3]==0x55aa00ff,"CMPR selector bit order and interpolation");
    put16(cmpr,0,0);put16(cmpr,2,0xffff);cmpr[4]=0xff;
    check(smg::decodeCmpr({cmpr.data(),cmpr.size()},8,8,texture,error) && !(texture.pixels[0]&255),"CMPR alpha mode");
    check(!smg::decodeCmpr({cmpr.data(),31},8,8,texture,error),"truncated CMPR rejected");
    check(!smg::decodeCmpr({cmpr.data(),32},2048,8,texture,error),"unsupported texture dimension rejected");
    std::printf("%u J3D model/texture checks passed\n",checks);return 0;
}
#ifndef SMG_TEST_RUNNER
int main() {return runModelTests();}
#endif
