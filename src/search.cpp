#include "search.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <ctime>
#endif

namespace cvrp {

double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("GetProcessTimes failed");
    const auto ticks = [](FILETIME x) {
        return (std::uint64_t{x.dwHighDateTime} << 32) | x.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
#else
    timespec t{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0) throw std::runtime_error("CPU clock failed");
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) * 1e-9;
#endif
}

Route ox(const Route& a, const Route& b, Node left, Node right) {
    const Node n = a.size();
    Route c(n);
    std::vector<bool> used(n + 1);
    for (Node i = left; i <= right; ++i) { c[i] = a[i]; used[a[i]] = true; }
    Node put = (right + 1) % n;
    for (Node j = 0; j < n; ++j) {
        const auto x = b[(right + 1 + j) % n];
        if (!used[x]) { c[put] = x; put = (put + 1) % n; }
    }
    return c;
}
void mutate(Route& seq, std::mt19937_64& rng) {
    if (seq.size() < 2) return;
    const Node a = std::uniform_int_distribution<Node>(0, seq.size()-1)(rng);
    Node b = std::uniform_int_distribution<Node>(0, seq.size()-2)(rng);
    if (b >= a) ++b;
    std::swap(seq[a], seq[b]);
}
namespace {
Route flatten(const Solution& s) {
    Route tour;
    for (const auto& r : s.routes) tour.insert(tour.end(), r.begin(), r.end());
    return tour;
}
struct Individual { Route tour; Solution sol; };
}

SearchResult search(const Instance& inst, const std::string& method, const SearchConfig& cfg,
                    const Observe& emit, const Stop& stop) {
    const bool multi = method == "multistart", ls = method == "ma" || multi;
    if (!ls && method != "ga") throw std::invalid_argument("unknown search method");
    if (cfg.population < 2 || cfg.steps == 0 || cfg.ls_cap == 0 || cfg.neighbors == 0)
        throw std::invalid_argument("search options must be positive and population >= 2");
    std::mt19937_64 rng(cfg.seed);
    const auto over = [&] { return stop && stop(); };
    Route base(inst.nodes.size()-1);
    std::iota(base.begin(), base.end(), Node{1});
    auto random_tour = [&] {
        auto p = base;
        std::shuffle(p.begin(), p.end(), rng);
        return p;
    };
    SearchResult result;
    result.best.cost = std::numeric_limits<Cost>::max();
    std::uint64_t step = 0;
    const auto observe = [&](const Solution& s) {
        if (s.cost < result.best.cost && !over()) {
            if (emit) emit(step, s);
            result.best = s;
        }
    };
    const auto neighbors = (ls && !over()) ? nearest(inst, cfg.neighbors) : Neighbors{};
    const auto improve = [&](Solution s) {
        LocalSearch engine(inst, neighbors, std::move(s));
        while (engine.evaluated() < cfg.ls_cap && !over()) {
            const Cost old = engine.solution().cost;
            if (!engine.advance()) break;
            if (engine.solution().cost < old) observe(engine.solution());
        }
        result.evaluations += engine.evaluated();
        return engine.solution();
    };
    if (multi) {
        for (std::uint64_t t = 0; t < cfg.steps && !over(); ++t) {
            step = t + 1;
            result.steps = step;
            auto sol = split(inst, random_tour());
            observe(sol);
            if (!over()) improve(std::move(sol));
        }
        return result;
    }
    std::vector<Individual> pop;
    pop.reserve(cfg.population);
    if (over()) return result;
    auto seed = savings(inst);
    observe(seed);
    pop.push_back({flatten(seed), std::move(seed)});
    while (pop.size() < cfg.population && !over()) {
        auto p = random_tour();
        auto s = split(inst, p);
        observe(s);
        pop.push_back({std::move(p), std::move(s)});
    }
    auto tournament = [&]() -> const Route& {
        std::uniform_int_distribution<Node> draw(0, pop.size()-1);
        const Node a = draw(rng), b = draw(rng);
        return pop[pop[a].sol.cost <= pop[b].sol.cost ? a : b].tour;
    };
    std::uniform_int_distribution<Node> index(0, base.size()-1);
    std::bernoulli_distribution mutation(0.2);
    for (std::uint64_t t = 0; t < cfg.steps && !over(); ++t) {
        step = t + 1;
        result.steps = step;
        const auto& a = tournament();
        const auto& b = tournament();
        const Node lo = index(rng), hi = index(rng);
        auto p = ox(a, b, std::min(lo, hi), std::max(lo, hi));
        if (mutation(rng)) mutate(p, rng);
        auto s = split(inst, p);
        observe(s);
        if (ls && !over()) {
            s = improve(std::move(s));
            p = flatten(s);
        }
        auto worst = pop.begin();
        for (auto it = pop.begin(); it != pop.end(); ++it)
            if (it->sol.cost >= worst->sol.cost) worst = it;
        // Keep insertion age as order. On equal cost discard the new child.
        if (s.cost < worst->sol.cost) {
            pop.erase(worst);
            pop.push_back({std::move(p), std::move(s)});
        }
    }
    return result;
}
}
