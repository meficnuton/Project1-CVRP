#include "search.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>

using namespace cvrp;
void require(bool b) { if (!b) throw std::runtime_error("search invariant failed"); }
void check(const Instance& i, const Solution& s) {
    std::stringstream f;
    write_solution(f, i, s);
    validate(f, i, IdMode::original);
}
Instance make(const std::vector<Customer>& ns, Cost q) {
    Instance i;
    i.capacity = q;
    i.nodes = ns;
    for (Node j = 0; j < ns.size(); ++j) i.index.emplace(ns[j].id, j);
    i.prepare_distances();
    return i;
}
int main() {
    std::mt19937_64 rng(123);
    Route a(12); std::iota(a.begin(), a.end(), Node{1});
    auto b = a;
    for (int j = 0; j < 200; ++j) {
        std::shuffle(b.begin(), b.end(), rng);
        const Node lo = j%6, hi = 6+j%6;
        auto c = ox(a,b,lo,hi);
        require(std::equal(c.begin()+lo,c.begin()+hi+1,a.begin()+lo));
        auto sorted = c; std::sort(sorted.begin(),sorted.end()); require(sorted==a);
        mutate(c,rng); sorted=c; std::sort(sorted.begin(),sorted.end()); require(sorted==a);
    }
    // Relocate empties a singleton source; swapping demand cannot pass a full destination.
    auto i = make({{10,0,0,0},{11,5,0,1},{12,6,0,1}},2);
    auto nn = nearest(i, 20);
    LocalSearch ls(i,nn,{{{1},{2}},22});
    require(ls.advance()); check(i,ls.solution()); require(ls.solution().routes.size()==1 && ls.solution().cost==12);
    auto full = make({{10,0,0,0},{11,5,0,2},{12,6,0,2}},2);
    LocalSearch reject(full,nn,{{{1},{2}},22});
    require(reject.advance() && reject.evaluated()==1 && reject.solution().cost==22);
    // Force SWAP to be the only improving neighborhood: both routes are full.
    auto is = make({{10,0,0,0},{11,10,0,1},{12,-10,0,1},{13,11,0,1},{14,-11,0,1}},2);
    auto ns = nearest(is,20);
    LocalSearch swaps(is,ns,{{{1,2},{3,4}},84});
    while (swaps.solution().cost==84 && swaps.advance()) {}
    check(is,swaps.solution()); require(swaps.solution().cost==44);
    // One route => only intra-route 2-opt; segments can touch a depot edge.
    auto il = make({{10,0,0,0},{11,1,0,1},{12,10,0,1},{13,2,0,1},{14,11,0,1}},4);
    auto nl = nearest(il,20);
    LocalSearch rev(il,nl,{{{1,2,3,4}},38});
    check(il,rev.solution());
    while (rev.advance()) check(il,rev.solution());
    require(rev.solution().cost==22 && rev.exhausted());
    int accepted = 0;
    for (int j=0;j<70;++j) {
        std::vector<Customer> pts{{100,0,0,0}};
        for (Node k=1;k<=12;++k)
            pts.push_back({static_cast<Id>(100+k),static_cast<Cost>(rng()%41)-20,
                           static_cast<Cost>(rng()%41)-20,static_cast<Cost>(1+rng()%4)});
        auto data=make(pts,12);
        auto candidates=nearest(data,8);
        std::shuffle(b.begin(),b.end(),rng);
        LocalSearch engine(data,candidates,split(data,b));
        auto prev=engine.solution().cost;
        while (engine.advance()) {
            const auto now=engine.solution().cost;
            require(now<=prev);
            if (now<prev) { ++accepted; check(data,engine.solution()); }
            prev=now;
        }
        check(data,engine.solution());
    }
    require(accepted>0);
    std::cout << "Checked 200 OX/mutations; validated " << accepted << " accepted random moves\n";
}
