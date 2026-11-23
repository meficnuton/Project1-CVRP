#include "search.hpp"
#include <algorithm>
#include <tuple>

namespace cvrp {
Neighbors nearest(const Instance& in, std::size_t k) {
    Neighbors list(in.nodes.size());
    for (Node i=1; i<in.nodes.size(); ++i) {
        auto& row=list[i];
        for (Node j=1; j<in.nodes.size(); ++j) if (j!=i) row.push_back(j);
        const auto cmp=[&](Node a, Node b) {
            return std::make_tuple(in.dist(i,a),in.nodes[a].id) < std::make_tuple(in.dist(i,b),in.nodes[b].id);
        };
        const auto size=std::min(k,row.size());
        std::partial_sort(row.begin(),row.begin()+size,row.end(),cmp);
        row.resize(size);
    }
    return list;
}
LocalSearch::LocalSearch(const Instance& in, const Neighbors& nn, Solution sol)
    : in_(in), nn_(nn), sol_(std::move(sol)), where_(in.nodes.size()) { refresh(); seek(); }
void LocalSearch::refresh() {
    loads_.assign(sol_.routes.size(),0);
    for (Node r=0;r<sol_.routes.size();++r)
        for (Node p=0;p<sol_.routes[r].size();++p) {
            const auto v=sol_.routes[r][p];
            where_[v]={r,p}; loads_[r]+=in_.nodes[v].demand;
        }
    a_=1; neighbor_=0; phase_=0; side_=0;
}
void LocalSearch::seek() {
    while (phase_<3) {
        if (a_>=in_.nodes.size()) { ++phase_; a_=1; neighbor_=0; side_=0; continue; }
        if (neighbor_>=nn_[a_].size()) { ++a_; neighbor_=0; side_=0; continue; }
        const auto b=nn_[a_][neighbor_];
        const bool same=where_[a_].first==where_[b].first;
        if (same==(phase_==2)) return;
        ++neighbor_; side_=0;
    }
    done_=true;
}
bool LocalSearch::advance() {
    if (done_) return false;
    const auto a=a_, b=nn_[a_][neighbor_];
    const auto [r,p]=where_[a]; const auto [s,q]=where_[b];
    auto& u=sol_.routes[r]; auto& v=sol_.routes[s];
    auto d=[&](Node x,Node y){return in_.dist(x,y);};
    const auto prev=[](const Route& rt,Node k){return k?rt[k-1]:Node{0};};
    const auto next=[](const Route& rt,Node k){return k+1<rt.size()?rt[k+1]:Node{0};};
    const auto da=in_.nodes[a].demand, db=in_.nodes[b].demand;
    Cost delta=0;
    ++evaluated_;
    if (phase_==0) {
        if (da<=in_.capacity-loads_[s]) {
            const auto x=prev(u,p),y=next(u,p);
            const Node at=q+side_, l=prev(v,at), h=at<v.size()?v[at]:0;
            delta=d(x,y)-d(x,a)-d(a,y)+d(l,a)+d(a,h)-d(l,h);
            if (delta<0) {
                v.insert(v.begin()+at,a);
                u.erase(u.begin()+p);
                if (u.empty()) sol_.routes.erase(sol_.routes.begin()+r);
            }
        }
    } else if (phase_==1) {
        if (db<=in_.capacity-(loads_[r]-da) && da<=in_.capacity-(loads_[s]-db)) {
            const auto x=prev(u,p),y=next(u,p),l=prev(v,q),h=next(v,q);
            delta=d(x,b)+d(b,y)+d(l,a)+d(a,h)-d(x,a)-d(a,y)-d(l,b)-d(b,h);
            if (delta<0) std::swap(u[p],v[q]);
        }
    } else {
        const Node lo=std::min(p,q), hi=std::max(p,q);
        const auto x=prev(u,lo), y=next(u,hi);
        delta=d(x,u[hi])+d(u[lo],y)-d(x,u[lo])-d(u[hi],y);
        if (delta<0) std::reverse(u.begin()+lo,u.begin()+hi+1);
    }
    if (delta<0) { sol_.cost+=delta; refresh(); }
    else if (phase_==0 && !side_) side_=1;
    else { ++neighbor_; side_=0; }
    seek();
    return true;
}
}
