#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace GX
{
    // Immutable world-space BVH for opaque, two-sided bake shadow blockers.
    // Picking and GI nearest-hit queries retain their existing semantics.
    class BakeOcclusion
    {
    public:
        using Point = std::array<double, 3>;
        // Keep the ray on its receiving triangle in double precision. A float
        // world-space interpolation can move it through a thin nearby blocker.
        struct SurfacePoint
        {
            Point a, e1, e2, origin;
            double nearDistance;
            SurfacePoint(const Point& A, const Point& B, const Point& C, double u, double v)
                : a(A)
            {
                double scale = 1.0;
                for (int i=0;i<3;++i)
                {
                    e1[i]=B[i]-A[i]; e2[i]=C[i]-A[i];
                    origin[i]=A[i]+u*e1[i]+v*e2[i];
                    scale=std::max(scale,std::abs(A[i])+std::abs(e1[i])+std::abs(e2[i]));
                }
                // Numerical roundoff only, not a fixed scene-space exclusion zone.
                nearDistance=64*std::numeric_limits<double>::epsilon()*scale;
            }
        };
        struct Stats { uint64_t nodes = 0, triangles = 0; };
    private:
        static Point sub(const Point& a, const Point& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
        static Point cross(const Point& a, const Point& b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
        static double dot(const Point& a, const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
        struct Box
        {
            Point lo{INFINITY,INFINITY,INFINITY}, hi{-INFINITY,-INFINITY,-INFINITY};
            void add(const Point& p) { for (int i=0;i<3;++i) { lo[i]=std::min(lo[i],p[i]); hi[i]=std::max(hi[i],p[i]); } }
            bool hit(const Point& o, const Point& d, double tMin, double tMax) const
            {
                for (int i=0;i<3;++i)
                {
                    if (d[i] == 0) { if (o[i]<lo[i] || o[i]>hi[i]) return false; continue; }
                    double a=(lo[i]-o[i])/d[i], b=(hi[i]-o[i])/d[i];
                    if (a>b) std::swap(a,b);
                    tMin=std::max(tMin,a); tMax=std::min(tMax,b);
                    if (tMin>tMax) return false;
                }
                return true;
            }
        };
        struct Triangle { Point a, e1, e2, center; Box bounds; };
        struct Node { Box bounds; size_t start=0, count=0, left=0, right=0; };
        std::vector<Triangle> triangles;
        std::vector<Node> nodes;
        std::function<void()> checkpoint;
        size_t split(size_t begin, size_t end)
        {
            if (checkpoint) checkpoint();
            const size_t index=nodes.size(); nodes.emplace_back();
            Box bounds, centers;
            for (size_t i=begin;i<end;++i) { bounds.add(triangles[i].bounds.lo); bounds.add(triangles[i].bounds.hi); centers.add(triangles[i].center); }
            nodes[index].bounds=bounds;
            if (end-begin<=8) { nodes[index].start=begin; nodes[index].count=end-begin; return index; }
            int axis=0;
            for (int i=1;i<3;++i) if (centers.hi[i]-centers.lo[i]>centers.hi[axis]-centers.lo[axis]) axis=i;
            const size_t middle=begin+(end-begin)/2;
            std::nth_element(triangles.begin()+begin, triangles.begin()+middle, triangles.begin()+end,
                [axis](const Triangle& a, const Triangle& b) { return a.center[axis]<b.center[axis]; });
            const size_t left=split(begin,middle), right=split(middle,end);
            nodes[index].left=left; nodes[index].right=right;
            return index;
        }
        static bool hitTriangle(const Triangle& t, const Point& o, const Point& d, double tMin, double tMax)
        {
            const Point p=cross(d,t.e2);
            const double det=dot(t.e1,p);
            if (det == 0 || !std::isfinite(det)) return false;
            const Point v=sub(o,t.a);
            const double u=dot(v,p)/det;
            if (u<0 || u>1) return false;
            const Point q=cross(v,t.e1);
            const double w=dot(d,q)/det;
            if (w<0 || u+w>1) return false;
            const double distance=dot(t.e2,q)/det;
            return distance>tMin && distance<tMax;
        }
        bool visit(size_t index, const Point& o, const Point& d, double tMin, double tMax, Stats* stats, const SurfacePoint* receiver=nullptr) const
        {
            const auto& n=nodes[index];
            if (stats) ++stats->nodes;
            if (!n.bounds.hit(o,d,tMin,tMax)) return false;
            if (n.count)
            {
                for (size_t i=n.start;i<n.start+n.count;++i)
                {
                    if (stats) ++stats->triangles;
                    const auto& t=triangles[i];
                    // Exclude only the source triangle, never the entire object:
                    // another triangle of the same mesh may be the thin blocker.
                    if (receiver && t.a==receiver->a && t.e1==receiver->e1 && t.e2==receiver->e2) continue;
                    if (hitTriangle(t,o,d,tMin,tMax)) return true;
                }
                return false;
            }
            return visit(n.left,o,d,tMin,tMax,stats,receiver) || visit(n.right,o,d,tMin,tMax,stats,receiver);
        }
    public:
        void add(const Point& a, const Point& b, const Point& c)
        {
            for (int i=0;i<3;++i) if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || !std::isfinite(c[i])) return;
            Triangle t; t.a=a; t.e1=sub(b,a); t.e2=sub(c,a);
            const auto normal=cross(t.e1,t.e2);
            if (dot(normal,normal)==0) return;
            t.bounds.add(a); t.bounds.add(b); t.bounds.add(c);
            for (int i=0;i<3;++i) t.center[i]=(a[i]+b[i]+c[i])/3;
            triangles.push_back(t);
        }
        void build(std::function<void()> progress = {})
        {
            checkpoint=std::move(progress); nodes.clear();
            if (!triangles.empty()) split(0,triangles.size());
            checkpoint={};
        }
        bool occludedFromSurface(const SurfacePoint& receiver, const Point& direction, double tMax) const
        {
            if (nodes.empty() || tMax<=receiver.nearDistance) return false;
            return visit(0,receiver.origin,direction,receiver.nearDistance,tMax,nullptr,&receiver);
        }
        bool occludedToLight(const SurfacePoint& receiver, const Point& light) const
        {
            Point direction=sub(light,receiver.origin);
            const double distance=std::sqrt(dot(direction,direction));
            if (!(distance>receiver.nearDistance)) return false;
            for (auto& x:direction) x/=distance;
            return occludedFromSurface(receiver,direction,std::nextafter(distance,0.0));
        }
        size_t triangleCount() const { return triangles.size(); }
        bool occluded(const Point& origin, const Point& direction, double tMin, double tMax, Stats* stats=nullptr) const
        {
            if (nodes.empty() || tMax<=tMin) return false;
            return visit(0,origin,direction,tMin,tMax,stats);
        }
    };
}
