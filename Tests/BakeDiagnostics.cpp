// CPU-only regression checks: no renderer, GPU, or owner project required.
#include "../Editor/Classes/BakeBlur.h"
#include "../Editor/Classes/BakeUV.h"
#include "../Editor/Classes/BakeOcclusion.h"
#include <limits>
#include "../Editor/Classes/BakeOutputTransaction.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

struct Pixel
{
    float r = 0, g = 0, b = 0, a = 0;
    Pixel() = default;
    Pixel(float R, float G, float B, float A) : r(R), g(G), b(B), a(A) {}
    Pixel operator*(float n) const { return Pixel(r*n, g*n, b*n, a*n); }
    Pixel& operator+=(const Pixel& v) { r+=v.r; g+=v.g; b+=v.b; a+=v.a; return *this; }
    Pixel& operator+=(float v) { r+=v; g+=v; b+=v; a+=v; return *this; }
};
constexpr float pi = 3.14159265358979323846f;

// Reference from the original baker: exp and normalization in the inner loop.
void reference(Pixel* lightmap, int size, float radius)
{
    std::vector<Pixel> data(size * size);
    for (int i = 0; i < size * size; ++i) data[i] = lightmap[i] * 255.0f;
    float rs = std::ceil(radius * 2.57f);
    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j)
        {
            Pixel val, wsum;
            for (int iy = int(i-rs); iy < i+rs+1; ++iy)
                for (int ix = int(j-rs); ix < j+rs+1; ++ix)
                {
                    int x = std::min(size-1, std::max(0, ix));
                    int y = std::min(size-1, std::max(0, iy));
                    float dsq = float((ix-j)*(ix-j)+(iy-i)*(iy-i));
                    float weight = std::exp(-dsq/(2*radius*radius))/(pi*2*radius*radius);
                    val += data[y*size+x]*weight; wsum += weight;
                }
            lightmap[i*size+j] = Pixel(std::round(val.r/wsum.r)/255.0f,
                std::round(val.g/wsum.g)/255.0f, std::round(val.b/wsum.b)/255.0f, 1.0f);
        }
}

void checkBlur()
{
    std::mt19937 random(12345);
    std::uniform_real_distribution<float> value(0, 1);
    for (int size : {1, 2, 7, 16, 64})
        for (int mode = 0; mode < 3; ++mode)
        {
            std::vector<Pixel> before(size*size);
            for (auto& p : before) p = mode == 0 ? Pixel(.2f,.5f,.9f,1) :
                mode == 1 ? Pixel(value(random), value(random), value(random),1) : Pixel();
            if (mode == 2) before[0] = Pixel(1, 1, 1, 1); // edge clamping
            auto old = before, optimized = before;
            for (int pass = 0; pass < 3; ++pass)
            {
                reference(old.data(), size, 2);
                int rows = 0;
                GX::bakeBlur(optimized.data(), size, 2, pi, [&] { ++rows; });
                assert(rows == size*2);
                for (size_t i = 0; i < old.size(); ++i)
                    assert(old[i].r == optimized[i].r && old[i].g == optimized[i].g &&
                        old[i].b == optimized[i].b && old[i].a == optimized[i].a);
            }
        }
    std::vector<Pixel> input(128*128, Pixel(.31f,.62f,.93f,1));
    auto old = input, optimized = input;
    auto begin = std::chrono::steady_clock::now();
    reference(old.data(), 128, 2);
    auto middle = std::chrono::steady_clock::now();
    GX::bakeBlur(optimized.data(), 128, 2, pi, [] {});
    auto end = std::chrono::steady_clock::now();
    assert(old[0].r == optimized[0].r);
    std::cout << "Blur PASS (exact RGB/A, repeated passes, edges, row checkpoints). 128x128 single pass: reference "
        << std::chrono::duration<double,std::milli>(middle-begin).count() << " ms, cached weights "
        << std::chrono::duration<double,std::milli>(end-middle).count() << " ms\n";
    bool cancelled = false;
    try { GX::bakeBlur(input.data(), 128, 2, pi, [] { throw std::runtime_error("cancel"); }); }
    catch (const std::runtime_error&) { cancelled = true; }
    assert(cancelled);
}

void write(const std::filesystem::path& p, const char* value) { std::ofstream f(p); f << value; assert(f.good()); }
std::string read(const std::filesystem::path& p) { std::ifstream f(p); std::string s; f >> s; return s; }
void checkTransaction(const std::filesystem::path& root)
{
    namespace fs = std::filesystem;
    fs::create_directories(root);
    auto target = root/"Lightmaps";
    fs::create_directory(target);
    write(target/"old.jpg", "original");
    fs::path stage;
    { // cancellation before publication
        GX::BakeOutputTransaction transaction(target);
        stage = transaction.directory();
        write(stage/"new.jpg", "partial");
    }
    assert(!fs::exists(stage) && read(target/"old.jpg") == "original");
    { // failed publication must restore the previous directory
        GX::BakeOutputTransaction transaction(target);
        fs::remove(transaction.directory()); // inject missing staged output
        bool failed = false;
        try { transaction.commit(); } catch (const fs::filesystem_error&) { failed = true; }
        assert(failed && read(target/"old.jpg") == "original");
    }
    {
        GX::BakeOutputTransaction transaction(target);
        write(transaction.directory()/"new.jpg", "complete");
        transaction.commit();
        assert(!fs::exists(target/"old.jpg") && read(target/"new.jpg") == "complete");
    }
    fs::remove_all(target);
    { // first bake, no preceding output
        GX::BakeOutputTransaction transaction(target);
        write(transaction.directory()/"first.jpg", "first");
        transaction.commit();
        assert(read(target/"first.jpg") == "first");
    }
    bool rejected = false;
    try { GX::BakeOutputTransaction transaction(root); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    fs::remove_all(root); // unique test fixture only
    std::cout << "Output transaction PASS (cancel, rollback, replacement, first bake, invalid target).\n";
}
void checkUV()
{
    struct UV { float x, y; };
    const UV a{0,0}, b{0.0001f,0}, c{0,0.0001f};
    const auto weights = GX::bakeBarycentric(a,b,c, UV{0.000025f,0.000025f});
    assert(std::abs(weights[0]-.5) < 1e-6 && std::abs(weights[1]-.25) < 1e-6 && std::abs(weights[2]-.25) < 1e-6);
    const auto reverse = GX::bakeBarycentric(a,c,b, UV{0.000025f,0.000025f});
    assert(std::abs(reverse[0]-.5) < 1e-6);
    assert(GX::bakeBarycentric(a,a,a,a)[0] < 0);
    assert(GX::bakeBarycentric(a,b,c,UV{1,1})[0] < 0);
    assert(!GX::bakeUvInRange(UV{2,0}));
    assert(!GX::bakeUvInRange(UV{std::numeric_limits<float>::quiet_NaN(),0}));
    assert(GX::bakeUvInRange(UV{1,0}));
    std::cout << "UV PASS (collapsed, small valid triangle, winding, outside, nonfinite).\n";
}
void checkOcclusion()
{
    using Tree=GX::BakeOcclusion; using P=Tree::Point;
    Tree wall;
    wall.add(P{-1,-1,0},P{1,-1,0},P{0,1,0}); wall.build();
    assert(wall.occluded(P{0,0,1},P{0,0,-1},.001,10));
    assert(wall.occluded(P{0,0,-1},P{0,0,1},.001,10)); // back of a paper-thin wall
    assert(!wall.occluded(P{0,0,-1},P{0,0,1},.001,.5)); // blocker beyond light
    assert(!wall.occluded(P{0,0,0},P{0,0,1},.001,10)); // self surface
    assert(!wall.occluded(P{0,0,1},P{1,0,0},.001,10)); // parallel
    assert(!wall.occluded(P{3,0,1},P{0,0,-1},.001,10)); // outside
    assert(wall.occluded(P{0,0,2000},P{0,0,-1},.001,INFINITY));
    Tree reversed; reversed.add(P{0,1,0},P{1,-1,0},P{-1,-1,0}); reversed.build();
    assert(reversed.occluded(P{0,0,1},P{0,0,-1},.001,10));
    Tree empty; empty.build(); assert(!empty.occluded(P{0,0,1},P{0,0,-1},0,10));

    // Independent plane/edge-test reference for randomized triangle scenes.
    auto sub=[](P a,P b) { return P{a[0]-b[0],a[1]-b[1],a[2]-b[2]}; };
    auto cross=[](P a,P b) { return P{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; };
    auto dot=[](P a,P b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    std::mt19937 rng(73); std::uniform_real_distribution<double> value(-10,10);
    auto point=[&] { return P{value(rng),value(rng),value(rng)}; };
    std::vector<std::array<P,3>> triangles;
    Tree random;
    for (int i=0;i<200;++i) { std::array<P,3> t{point(),point(),point()}; triangles.push_back(t); random.add(t[0],t[1],t[2]); }
    random.build();
    for (int i=0;i<1000;++i)
    {
        P origin=point(), direction=point(); const double length=std::sqrt(dot(direction,direction));
        for (auto& v:direction) v/=length;
        double far=i%2 ? 5 : 100;
        bool referenceHit=false;
        for (auto& t:triangles)
        {
            P n=cross(sub(t[1],t[0]),sub(t[2],t[0]));
            const double denom=dot(n,direction);
            if (denom==0) continue;
            const double distance=dot(n,sub(t[0],origin))/denom;
            if (distance<=.001 || distance>=far) continue;
            P hit{origin[0]+distance*direction[0],origin[1]+distance*direction[1],origin[2]+distance*direction[2]};
            bool inside=true;
            for (int edge=0;edge<3;++edge)
                if (dot(n,cross(sub(t[(edge+1)%3],t[edge]),sub(hit,t[edge])))<0) inside=false;
            if (inside) { referenceHit=true; break; }
        }
        assert(random.occluded(origin,direction,.001,far)==referenceHit);
    }
    Tree grid;
    for (int x=0;x<64;++x) for (int y=0;y<64;++y)
    {
        grid.add(P{double(x),double(y),0},P{double(x+1),double(y),0},P{double(x),double(y+1),0});
        grid.add(P{double(x+1),double(y),0},P{double(x+1),double(y+1),0},P{double(x),double(y+1),0});
    }
    grid.build(); Tree::Stats stats;
    assert(grid.occluded(P{32.2,32.3,10},P{0,0,-1},.001,20,&stats));
    assert(stats.triangles<64);
    std::cout << "Occlusion PASS (two sides, thin wall, range, self, parallel, winding, empty, 1000 reference rays). BVH tested "
        << stats.triangles << " of " << grid.triangleCount() << " triangles for grid ray.\n";
}
int main()
{
    checkOcclusion();
    checkUV();
    checkBlur();
    checkTransaction(std::filesystem::temp_directory_path() /
        ("falco-bake-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
}
