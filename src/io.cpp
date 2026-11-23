#include "cvrp.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cvrp {
namespace {
[[noreturn]] void fail(const std::string& s) { throw std::runtime_error(s); }
std::string trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
Cost integer(std::string_view s, const std::string& context) {
    Cost z{};
    const auto r = std::from_chars(s.data(), s.data() + s.size(), z);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) fail("invalid integer: " + context);
    return z;
}
Cost reported_cost(std::string_view s, IdMode mode) {
    if (mode == IdMode::cvrplib_x) {
        const auto dot = s.find('.');
        if (dot != std::string_view::npos) {
            const auto frac = s.substr(dot + 1);
            if (frac.empty() || frac.find_first_not_of('0') != std::string_view::npos)
                fail("invalid integer: cost");
            s = s.substr(0, dot);
        }
    }
    return integer(s, "cost");
}
std::vector<Cost> numbers(const std::string& s, const std::string& context) {
    std::vector<Cost> a;
    std::istringstream in(s);
    for (std::string w; in >> w;) a.push_back(integer(w, context));
    return a;
}
Cost edge(const Customer& a, const Customer& b) {
    // Input bounds ensure 4*s and the adjacent squares fit int64.
    const Cost dx = a.x - b.x, dy = a.y - b.y;
    const Cost u = 4 * (dx * dx + dy * dy);
    Cost k = static_cast<Cost>(std::sqrt(static_cast<double>(u)));
    while ((k + 1) * (k + 1) <= u) ++k;
    while (k * k > u) --k;
    return (k + 1) / 2;
}
}

Instance read_instance(std::istream& in) {
    enum Stage { header, coord, demand, depot, done };
    Stage stage = header;
    std::map<std::string, std::string> headers;
    std::map<Id, std::pair<Cost, Cost>> points;
    std::map<Id, Cost> demands;
    std::vector<Id> depots;
    bool depot_end = false;
    Cost dim = 0, capacity = 0;
    for (std::string line; std::getline(in, line);) {
        const auto s = trim(line);
        if (s.empty()) continue;
        if (stage == done) fail("unexpected content after EOF");
        if (s == "NODE_COORD_SECTION" && stage == header) {
            if (headers["TYPE"] != "CVRP") fail("TYPE must be CVRP");
            if (headers["EDGE_WEIGHT_TYPE"] != "EUC_2D") fail("unsupported EDGE_WEIGHT_TYPE: expected EUC_2D");
            dim = integer(headers["DIMENSION"], "DIMENSION");
            capacity = integer(headers["CAPACITY"], "CAPACITY");
            if (dim < 2 || dim > 3000) fail("DIMENSION out of range [2,3000]");
            if (capacity <= 0 || capacity > 1'000'000'000'000LL) fail("CAPACITY out of supported range");
            stage = coord;
        } else if (s == "DEMAND_SECTION" && stage == coord) {
            stage = demand;
        } else if (s == "DEPOT_SECTION" && stage == demand) {
            stage = depot;
        } else if (s == "EOF" && stage == depot && depot_end) {
            stage = done;
        } else if (stage == header) {
            const auto sep = s.find(':');
            if (sep == std::string::npos) fail("unsupported header or section: " + s);
            const auto key = trim(s.substr(0, sep));
            if (key == "COMMENT") continue;
            if (key != "NAME" && key != "TYPE" && key != "EDGE_WEIGHT_TYPE" && key != "DIMENSION" && key != "CAPACITY")
                fail("unsupported header: " + key);
            if (!headers.emplace(key, trim(s.substr(sep + 1))).second) fail("duplicate header: " + key);
        } else if (stage == coord) {
            const auto a = numbers(s, "coordinate row");
            if (a.size() != 3 || a[0] <= 0 || a[1] < -1'000'000 || a[1] > 1'000'000 || a[2] < -1'000'000 || a[2] > 1'000'000)
                fail("invalid coordinate row or value out of supported range");
            if (!points.emplace(a[0], std::make_pair(a[1], a[2])).second) fail("duplicate coordinate ID");
            if (points.size() > static_cast<Node>(dim)) fail("coordinate count exceeds DIMENSION");
        } else if (stage == demand) {
            const auto a = numbers(s, "demand row");
            if (a.size() != 2 || a[0] <= 0 || a[1] < 0 || a[1] > capacity) fail("invalid demand row or demand exceeds CAPACITY");
            if (!demands.emplace(a[0], a[1]).second) fail("duplicate demand ID");
            if (demands.size() > static_cast<Node>(dim)) fail("demand count exceeds DIMENSION");
        } else {
            if (depot_end) fail("unexpected data after depot terminator");
            const auto a = numbers(s, "depot row");
            if (a.size() != 1) fail("expected one depot entry per row");
            if (a[0] == -1) depot_end = true;
            else if (a[0] <= 0 || !depots.empty()) fail("exactly one positive depot ID is supported");
            else depots.push_back(a[0]);
        }
    }
    if (in.bad()) fail("instance input I/O error");
    if ((stage != depot && stage != done) || !depot_end || depots.empty()) fail("missing depot section or terminator");
    if (points.size() != static_cast<Node>(dim) || demands.size() != points.size()) fail("row count does not match DIMENSION");
    const Id hub = depots.front();
    if (!points.count(hub) || !demands.count(hub) || demands.at(hub) != 0) fail("depot must exist with demand 0");
    Instance inst;
    inst.capacity = capacity;
    auto append = [&](Id id) {
        const auto [x, y] = points.at(id);
        if (!demands.count(id)) fail("missing demand for coordinate ID");
        const auto q = demands.at(id);
        if (id != hub && q == 0) fail("customer demand must be positive");
        inst.index.emplace(id, inst.nodes.size());
        inst.nodes.push_back({id, x, y, q});
    };
    append(hub);
    for (const auto& p : points) if (p.first != hub) append(p.first);
    return inst;
}

void Instance::prepare_distances() {
    const Node n = nodes.size();
    distances.assign(n * n, 0);
    for (Node i = 0; i < n; ++i)
        for (Node j = i + 1; j < n; ++j)
            distances[i*n+j] = distances[j*n+i] = edge(nodes[i], nodes[j]);
}

Solution validate(std::istream& in, const Instance& inst, IdMode mode) {
    const Node n = inst.nodes.size();
    if (mode == IdMode::cvrplib_x) {
        if (inst.nodes.front().id != 1) fail("cvrplib-x requires depot ID 1");
        for (Node i = 1; i <= n; ++i)
            if (!inst.index.count(static_cast<Id>(i))) fail("cvrplib-x requires contiguous original IDs 1..DIMENSION");
    }
    Solution sol;
    std::vector<bool> seen(n);
    std::size_t served = 0;
    bool got_cost = false;
    Cost reported = -1;
    for (std::string line; std::getline(in, line);) {
        const auto s = trim(line);
        if (s.empty()) continue;
        if (got_cost) fail("unexpected content after cost");
        if (s.rfind("Route #", 0) == 0) {
            const auto sep = s.find(':');
            if (sep == std::string::npos || sep < 8 ||
                integer(s.substr(7, sep - 7), "route number") != static_cast<Cost>(sol.routes.size() + 1))
                fail("expected sequential route number");
            const auto ids = numbers(s.substr(sep + 1), "customer ID");
            if (ids.empty()) fail("empty route");
            Route r;
            Cost load = 0;
            Node prev = 0;
            for (Id id : ids) {
                if (mode == IdMode::cvrplib_x) {
                    if (id <= 0 || id >= static_cast<Id>(n)) fail("unknown cvrplib-x customer ID");
                    ++id;
                }
                const auto pos = inst.index.find(id);
                if (pos == inst.index.end()) fail("unknown node ID");
                const Node j = pos->second;
                if (j == 0) fail("depot must not be listed inside a route");
                if (seen[j]) fail("duplicate customer");
                seen[j] = true;
                ++served;
                const Cost q = inst.nodes[j].demand;
                if (q > inst.capacity - load) fail("route exceeds capacity");
                load += q;
                sol.cost += edge(inst.nodes[prev], inst.nodes[j]);
                r.push_back(j);
                prev = j;
            }
            sol.cost += edge(inst.nodes[prev], inst.nodes[0]);
            sol.routes.push_back(std::move(r));
        } else {
            std::istringstream row(s);
            std::string keyword, token, extra;
            if (!(row >> keyword >> token) || keyword != "Cost" || (row >> extra)) fail("expected route or cost line");
            reported = reported_cost(token, mode);
            got_cost = true;
        }
    }
    if (in.bad()) fail("solution input I/O error");
    if (!got_cost) fail("missing cost line");
    if (served != n-1) fail("missing customer");
    if (reported != sol.cost) fail("reported cost mismatch: computed " + std::to_string(sol.cost));
    return sol;
}

void write_solution(std::ostream& out, const Instance& inst, const Solution& sol) {
    std::size_t i = 0;
    for (const auto& r : sol.routes) {
        out << "Route #" << ++i << ':';
        for (Node j : r) out << ' ' << inst.nodes.at(j).id;
        out << '\n';
    }
    out << "Cost " << sol.cost << '\n';
    if (!out) fail("solution output I/O error");
}
} // namespace cvrp
