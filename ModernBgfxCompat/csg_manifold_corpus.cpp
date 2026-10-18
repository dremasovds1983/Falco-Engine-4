#include "../Engine/Renderer/CSGManifold.h"
#include "../out/falco/csg-corpus/brush-fixtures.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <set>
#include <fstream>

using namespace GX::CSGManifold;
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
Brush primitive(int type, Position scale, Position offset, uint64_t id, bool subtract = false) {
    CSGBrush source;
    if (type == 0) source.makeCube();
    if (type == 1) source.makeCylinder();
    if (type == 2) source.makeSphere();
    if (type == 3) source.makeCone();
    Brush b;
    b.subtract = subtract;
    b.reverseWinding = scale[0]*scale[1]*scale[2] < 0;
    for (auto v : source.vertices) b.positions.push_back({v.x*scale[0]+offset[0],v.y*scale[1]+offset[1],v.z*scale[2]+offset[2]});
    for (size_t i=0;i<source.faces.size();++i) {
        Face f;
        f.indices = source.faces[i].indices;
        // Unique face material proves attribution at splits and cut surfaces.
        f.attributes = {static_cast<uintptr_t>(i+1),id,static_cast<int>(id%31),(i%2)==0,(i%3)==0};
        for (auto uv : source.faces[i].texCoords) f.uv.push_back({uv.x*2+.125,uv.y*3-.25});
        b.faces.push_back(f);
    }
    return b;
}
Position sub(Position a,Position b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
double dot(Position a,Position b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
Position cross(Position a,Position b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
#ifdef FALCO_CSG_LEGACY_REFERENCE
#include "csg_legacy_reference.h"
#endif
bool interpolated(const Brush& b,const Face& f,Position p,UV uv) {
    for(size_t k=1;k+1<f.indices.size();++k) {
        auto a=b.positions[f.indices[0]], v0=sub(b.positions[f.indices[k]],a),v1=sub(b.positions[f.indices[k+1]],a),v2=sub(p,a);
        auto n=cross(v0,v1);
        if(std::abs(dot(n,v2))>1e-6*std::sqrt(dot(n,n)))continue;
        double d00=dot(v0,v0),d01=dot(v0,v1),d11=dot(v1,v1),d20=dot(v2,v0),d21=dot(v2,v1);
        double det=d00*d11-d01*d01;
        if(det<=0)continue;
        double v=(d11*d20-d01*d21)/det,w=(d00*d21-d01*d20)/det,u=1-v-w;
        if(std::min({u,v,w}) < -1e-5)continue;
        bool match=true;
        for(int j=0;j<2;++j)match &= std::abs(uv[j]-(u*f.uv[0][j]+v*f.uv[k][j]+w*f.uv[k+1][j]))<1e-5;
        if(match)return true;
    }
    return false;
}
std::string serialize(const std::vector<Triangle>& ts) {
    std::ostringstream out; out<<std::hexfloat;
    for(const auto& t:ts) {
        out<<t.attributes.material<<' '<<t.attributes.brush<<' '<<t.attributes.layer<<' '<<t.attributes.castShadows<<' '<<t.attributes.smoothNormals<<' ';
        for(int c=0;c<3;++c) {for(auto v:t.positions[c])out<<v<<' ';for(auto v:t.uv[c])out<<v<<' ';}out<<'\n';
    }
    return out.str();
}
std::string run(const std::string& name,const std::vector<Brush>& inputs,double volume,int euler) {
    Builder builder; std::string error;
    for(const auto& b:inputs)require(builder.apply(b,error),name+": "+error);
    require(std::abs(builder.geometry().Volume()-volume) <= std::max(1e-8,volume*.005),name+": volume");
    std::vector<Triangle> ts; require(builder.triangles(ts,error),name+": output "+error);
    // Weld only topology (properties remain discontinuous); every edge must
    // have two opposite directed uses, and the expected Euler characteristic.
    auto mesh=builder.geometry().GetMeshGL64();
    std::vector<uint64_t> parent(mesh.NumVert());for(uint64_t i=0;i<parent.size();++i)parent[i]=i;
    for(size_t i=0;i<mesh.mergeFromVert.size();++i)parent[mesh.mergeFromVert[i]]=mesh.mergeToVert[i];
    auto root=[&](uint64_t i){while(parent[i]!=i)i=parent[i];return i;};
    std::map<std::pair<uint64_t,uint64_t>,std::pair<int,int>> edges; std::set<uint64_t> vertices;
    for(size_t i=0;i<mesh.triVerts.size();i+=3)for(int c=0;c<3;++c) {
        auto a=root(mesh.triVerts[i+c]),b=root(mesh.triVerts[i+(c+1)%3]);
        require(a!=b,name+": degenerate edge");vertices.insert(a);
        auto& e=edges[std::minmax(a,b)];++e.first;e.second+=a<b?1:-1;
    }
    for(auto e:edges)require(e.second.first==2&&e.second.second==0,name+": open/nonmanifold edge");
    require(static_cast<int64_t>(vertices.size())-static_cast<int64_t>(edges.size())+mesh.NumTri()==euler,name+": Euler");
    for(const auto& t:ts) {
        auto b=std::find_if(inputs.begin(),inputs.end(),[&](const Brush& b){return b.faces[0].attributes.brush==t.attributes.brush;});
        require(b!=inputs.end(),name+": brush ID lost");
        const auto& f=b->faces.at(t.attributes.material-1);
        require(t.attributes.layer==f.attributes.layer&&t.attributes.castShadows==f.attributes.castShadows&&t.attributes.smoothNormals==f.attributes.smoothNormals,name+": discrete attribute changed");
        for(int c=0;c<3;++c)require(interpolated(*b,f,t.positions[c],t.uv[c]),name+": UV interpolation");
    }
    std::cout<<name<<" PASS volume="<<builder.geometry().Volume()<<" triangles="<<ts.size()<<" Euler="<<euler<<'\n';
    return serialize(ts);
}
int main(int argc, char** argv) {
    try {
        const uint64_t id=0xfedcba9876543210ull;
        auto cube=primitive(0,{1,1,1},{0,0,0},id);
        auto large=primitive(0,{2,2,2},{0,0,0},id);
        run("C1",{cube,primitive(0,{1,1,1},{1,1,1},id+1)},15,2);
        run("C2",{large,primitive(0,{.5,3,.5},{0,0,0},id+1,true)},60,0);
        run("C3",{large,primitive(1,{.5,3,.5},{0,0,0},id+1,true)},64-M_PI,0);
        run("C4",{cube,primitive(0,{1,1,1},{2,0,0},id+1)},16,2);
#ifdef FALCO_CSG_LEGACY_REFERENCE
        compareLegacy("C1",{cube,primitive(0,{1,1,1},{1,1,1},id+1)},15);
        compareLegacy("C2",{large,primitive(0,{.5,3,.5},{0,0,0},id+1,true)},60);
        compareLegacy("C3",{large,primitive(1,{.5,3,.5},{0,0,0},id+1,true)},64-M_PI);
        compareLegacy("C4",{cube,primitive(0,{1,1,1},{2,0,0},id+1)},16);
#endif
        std::vector<Brush> stairs;
        for(int i=0;i<6;++i)stairs.push_back(primitive(0,{1,.25,.5*(6-i)},{0,i*.5,.5*(6-i)-3+.5},id+i));
        auto first=run("C5",stairs,21,2);
#ifdef FALCO_CSG_LEGACY_REFERENCE
        compareLegacy("C5",stairs,21);
#endif
        std::vector<Brush> chain;
        for(int i=0;i<10;++i)chain.push_back(primitive(0,{1,1,1},{static_cast<double>(i),0,0},id+i));
        run("C6",chain,44,2);
#ifdef FALCO_CSG_LEGACY_REFERENCE
        compareLegacy("C6",chain,44);
#endif
        run("EdgeContact",{cube,primitive(0,{1,1,1},{2,2,0},id+1)},16,4);
        run("NestedCavity",{large,primitive(0,{1,1,1},{0,0,0},id+1,true)},56,4);
        std::vector<Brush> mixed={primitive(0,{10,2,2},{0,0,0},id)};
        for(int i=0;i<10;++i)mixed.push_back(primitive(0,{.25,3,.25},{-9.0+2*i,0,0},id+i+1,true));
        run("TenSubtractions",mixed,310,-18);
        Builder b;std::string error; require(b.apply(cube,error),error);
        std::vector<Brush> invalid={primitive(0,{1,0,1},{0,0,0},id+1),cube,cube,cube};
        invalid[1].faces.pop_back();invalid[2].faces[0].indices[0]=999;invalid[3].positions[0][0]=std::numeric_limits<double>::quiet_NaN();
        for(const auto& bad:invalid){require(!b.apply(bad,error)&&!error.empty(),"C7: invalid input accepted");require(std::abs(b.geometry().Volume()-8)<1e-8,"C7: previous result changed");}
        auto cut=cube;cut.subtract=true;require(b.apply(cut,error)&&b.geometry().IsEmpty(),"C7: complete subtraction");
        Builder empty;require(empty.apply(cut,error)&&empty.geometry().IsEmpty(),"C7: first subtraction");
        std::cout<<"C7 PASS invalid inputs skipped, valid empty results accepted\n";
        auto second=run("C8",stairs,21,2);require(first==second,"C8: nondeterministic geometry/properties");
        if(argc==2) {std::ofstream out(argv[1],std::ios::binary);out<<first;require(out.good(),"C8: cannot write serialization");}
        run("Mirror",{primitive(0,{-1,1,1},{0,0,0},id)},8,2);
        // Segmented primitive volumes include the expected tessellation error.
        double sphereVolume=0;
        for(int i=0;i<16;++i) {
            double a=M_PI*i/16,b=M_PI*(i+1)/16;
            double r0=std::sin(a),r1=std::sin(b);
            sphereVolume+=128*std::sin(2*M_PI/128)/6*(std::cos(a)-std::cos(b))*(r0*r0+r0*r1+r1*r1);
        }
        run("Sphere",{primitive(2,{1,1,1},{0,0,0},id)},sphereVolume,2);
        run("Cone",{primitive(3,{1,1,1},{0,0,0},id)},2.0/3*M_PI,2);
        std::cout<<"PASS: C1-C8 CPU corpus and additional primitives\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
