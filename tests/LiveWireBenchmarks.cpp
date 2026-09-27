#include "imageeditor/core/LiveWire.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace imageeditor::core;
using Clock=std::chrono::steady_clock;
static double milliseconds(Clock::duration d)
{ return std::chrono::duration<double,std::milli>(d).count(); }

int main()
{
    for (const Extent2u extent:{Extent2u{3840,2160},Extent2u{5120,2880}}) {
        const double center=double(extent.width)/2;
        MagneticEdgeCache cache(extent,[center](int x,int y){
            const int noise=((x*17+y*13)%9)-4;
            const auto v=std::uint8_t(std::clamp((x<center?45:225)+noise,0,255));
            return Rgba8{v,v,v,255};
        });
        for (bool warm:{false,true}) {
            const std::array guide {Vec2d{center-2.25,800.5},Vec2d{center-1.75,1000.5},Vec2d{center-2.25,1200.5}};
            const auto started=Clock::now();
            LiveWireSearch search(cache,guide,12);
            const auto constructed=Clock::now();
            unsigned steps=0;
            double maxStep=0;
            while(search.result()==LiveWireSearch::Result::Searching) {
                const auto before=Clock::now();
                search.step(512);
                maxStep=std::max(maxStep,milliseconds(Clock::now()-before));
                ++steps;
            }
            const auto completed=Clock::now();
            std::cout<<std::fixed<<std::setprecision(3)<<extent.width<<'x'<<extent.height
                     <<(warm?" warm":" cold")<<" prepare_ms="<<milliseconds(constructed-started)
                     <<" solve_ms="<<milliseconds(completed-constructed)<<" max_step_ms="<<maxStep
                     <<" steps="<<steps<<" path_points="<<search.path().size()
                     <<" prepared="<<search.stats().prepared<<" expanded="<<search.stats().expanded
                     <<" peak_queue="<<search.stats().peakQueue<<" samples="<<cache.stats().samples
                     <<" cache_MiB="<<double(cache.memoryBytes())/(1024*1024)
                     <<" search_MiB="<<double(search.memoryBytes())/(1024*1024)<<'\n';
            if(search.result()!=LiveWireSearch::Result::Edge) return 1;
        }
    }
}
