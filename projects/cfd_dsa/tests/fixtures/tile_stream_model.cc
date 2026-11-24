#include "arch/arm/cfd_tile_stream.hh"

#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <vector>

struct Result
{
    unsigned packets;
    std::vector<unsigned> ready;
};

Result simulate(unsigned cells, unsigned width, bool cellOrder)
{
    const std::array<unsigned, 2> sizes{40*cells, 200*cells};
    std::array<unsigned, 2> issued{};
    Result result{0, std::vector<unsigned>(cells)};
    while (issued != sizes) {
        unsigned region = cellOrder ? gem5::ArmISA::CfdTileStream::firstRegion(
            issued[0], issued[1], sizes[0], sizes[1]) : result.packets%2;
        if (issued[region] == sizes[region]) region = 1-region;
        assert(issued[region] < sizes[region]);
        issued[region] += std::min(width, sizes[region]-issued[region]);
        ++result.packets;
        for (unsigned cell=0; cell<cells; ++cell) {
            if (!result.ready[cell] && issued[0]>=40*(cell+1) &&
                    issued[1]>=200*(cell+1)) result.ready[cell]=result.packets;
        }
    }
    assert(result.packets == (sizes[0]+width-1)/width +
                            (sizes[1]+width-1)/width);
    assert(std::all_of(result.ready.begin(), result.ready.end(),
                      [](unsigned n) { return n>0; }));
    return result;
}

int main()
{
    using gem5::ArmISA::CfdTileStream::firstRegion;
    assert(firstRegion(0, 0, 640, 3200)==0);
    assert(firstRegion(64, 0, 640, 3200)==1);
    assert(firstRegion(64, 320, 640, 3200)==0);
    assert(firstRegion(640, 1000, 640, 3200)==1);
    assert(firstRegion(200, 3200, 640, 3200)==0);
    assert(firstRegion(0, 0, 640, 0)==0);
    for (unsigned cells : {1, 2, 16, 32}) {
        for (unsigned width : {8, 16, 32, 40, 64}) {
            const auto old=simulate(cells,width,false);
            const auto now=simulate(cells,width,true);
            assert(old.packets==now.packets);
            assert(now.ready.front()<=old.ready.front());
        }
    }
    const auto old=simulate(16,64,false), now=simulate(16,64,true);
    assert(old.packets==60 && now.packets==60);
    assert(old.ready.front()==8 && now.ready.front()==5);
    std::cout << "TILE_STREAM_MODEL_PASS packets=60 first_cell=8->5\n";
}
