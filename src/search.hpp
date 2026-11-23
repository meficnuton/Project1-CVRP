#pragma once
#include "cvrp.hpp"
#include <functional>
#include <random>

namespace cvrp {
using Neighbors = std::vector<Route>;
Neighbors nearest(const Instance&, std::size_t k);
using Observe = std::function<void(std::uint64_t, const Solution&)>;
// Resumable first-improvement sweep. `advance` consumes at most one candidate.
class LocalSearch {
public:
    LocalSearch(const Instance&, const Neighbors&, Solution);
    bool advance();
    bool exhausted() const { return done_; }
    std::uint64_t evaluated() const { return evaluated_; }
    const Solution& solution() const { return sol_; }
private:
    const Instance& in_;
    const Neighbors& nn_;
    Solution sol_;
    std::vector<Cost> loads_;
    std::vector<std::pair<Node, Node>> where_;
    Node a_{1}, neighbor_{};
    unsigned phase_{}, side_{};
    std::uint64_t evaluated_{};
    bool done_{};
    void refresh();
    void seek();
};
Route ox(const Route&, const Route&, Node left, Node right);
void mutate(Route&, std::mt19937_64&);
struct SearchConfig {
    std::uint64_t seed{1};
    std::uint64_t steps{100};
    std::uint64_t ls_cap{1000};
    std::size_t population{24};
    std::size_t neighbors{20};
};
struct SearchResult { Solution best; std::uint64_t evaluations{}; std::uint64_t steps{}; };
using Stop = std::function<bool()>;
SearchResult search(const Instance&, const std::string& method, const SearchConfig&, const Observe&, const Stop& stop = {});
double process_cpu_seconds();
}
