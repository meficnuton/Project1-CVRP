#pragma once
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace cvrp {
using Cost = std::int64_t;
using Id = std::int64_t;
using Node = std::size_t; // 0 = depot
using Route = std::vector<Node>;
struct Customer { Id id; Cost x, y, demand; };
struct Instance {
    Cost capacity{};
    std::vector<Customer> nodes;
    std::map<Id, Node> index;
    // Row-major, symmetric. Populated only on the solver path.
    std::vector<Cost> distances;
    Cost dist(Node i, Node j) const { return distances[i * nodes.size() + j]; }
    void prepare_distances();
};
struct Solution { std::vector<Route> routes; Cost cost{}; };
enum class IdMode { original, cvrplib_x };
Instance read_instance(std::istream& in);
Solution validate(std::istream& in, const Instance& inst, IdMode mode);
void write_solution(std::ostream& out, const Instance& inst, const Solution& sol);
Solution split(const Instance& inst, const Route& tour);
Solution savings(const Instance& inst);
} // namespace cvrp
