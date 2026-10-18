#include "../Editor/Classes/BakeOcclusion.h"
#include <cassert>
#include <iostream>

using Tree = GX::BakeOcclusion;
using P = Tree::Point;

int main()
{
    // A zero-thickness wall intersects a floor, rather than standing above it.
    // Test both windings and translated worlds. The old 0.001 near distance
    // must miss a blocker that the surface query now detects.
    int cases=0;
    for (double translation : {0.0, 100.0, 10000.0})
    for (double gap : {0.00001, 0.0001, 0.0005, 0.002})
    for (bool reverse : {false,true})
    {
        const P a{translation-2,0,-2}, b{translation+2,0,-2}, c{translation,0,2};
        Tree floor;
        floor.add(a,b,c); floor.build();
        Tree::SurfacePoint sample(a,b,c,.25,.5);
        const P direction{0.8,0.6,0};
        assert(!floor.occludedFromSurface(sample,direction,100));
        assert(!floor.occludedToLight(sample,P{translation+1,1,0}));

        const P w0{translation+gap,-1,-1}, w1{translation+gap,1,-1}, w2{translation+gap,0,1};
        Tree scene;
        scene.add(a,b,c);
        scene.add(w0,reverse?w2:w1,reverse?w1:w2);
        scene.build();
        assert(scene.occludedFromSurface(sample,direction,100));
        assert(scene.occludedToLight(sample,P{translation+1,1,0}));
        assert(!scene.occludedToLight(sample,P{translation+gap*.25,gap*.25,0}));
        assert(!scene.occludedFromSurface(sample,P{-0.8,0.6,0},100));
        if (gap/.8 < .001)
            assert(!scene.occluded(sample.origin,direction,.001,100));
        ++cases;
    }

    // Non-axis-aligned receiver with float-style barycentric weights whose sum
    // need not be exactly one. Source exclusion must survive BVH partitioning.
    const P a{431.13,91.72,-85.1}, b{435.2,95.19,-82.3}, c{429.73,97.81,-83.97};
    Tree tilted;
    tilted.add(a,b,c);
    for (int i=0;i<100;++i)
        tilted.add(P{double(i),-100,0},P{double(i)+.2,-100,0},P{double(i),-100,.2});
    tilted.build();
    for (int i=1;i<50;++i)
    {
        Tree::SurfacePoint sample(a,b,c,float(i/100.0),float(.25));
        assert(!tilted.occludedFromSurface(sample,P{0,1,0},100));
        assert(!tilted.occludedToLight(sample,sample.origin));
    }
    Tree empty;
    empty.build();
    assert(!empty.occludedFromSurface(Tree::SurfacePoint(a,b,c,.2,.3),P{0,1,0},100));
    std::cout << "Shadow contact PASS: " << cases
              << " wall/floor cases, winding, translations, finite lights, self hits and BVH reorder.\n";
}
