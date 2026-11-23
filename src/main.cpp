#include "cvrp.hpp"
#include "search.hpp"
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {
using namespace cvrp;
namespace fs = std::filesystem;
constexpr auto usage = "Usage:\n"
    "  cvrp solve INSTANCE --method savings|split|ga|multistart|ma --out FILE\n"
    "    split: --tour ID,ID,...\n"
    "    ga/multistart/ma: [--steps N] [--cpu-seconds S] [--seed N]\n"
    "      [--population N] [--neighbors N] [--ls-cap N] [--log FILE]\n"
    "  cvrp validate INSTANCE FILE [--ids original|cvrplib-x]\n";

std::ifstream open_input(const char* name) {
    std::ifstream in(name);
    if (!in) throw std::runtime_error(std::string("cannot open input: ") + name);
    return in;
}
Route parse_tour(std::string_view text, const Instance& inst) {
    Route r;
    for (;;) {
        const auto comma = text.find(',');
        const auto token = text.substr(0, comma);
        Id id{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
            throw std::runtime_error("invalid tour ID");
        const auto it = inst.index.find(id);
        if (it == inst.index.end() || it->second == 0) throw std::runtime_error("unknown customer in tour");
        r.push_back(it->second);
        if (r.size() >= inst.nodes.size()) throw std::runtime_error("too many IDs in tour");
        if (comma == std::string_view::npos) return r;
        text.remove_prefix(comma+1);
    }
}
std::uint64_t count_arg(std::string_view s) {
    std::uint64_t v{};
    const auto p = std::from_chars(s.data(), s.data()+s.size(), v);
    if (p.ec != std::errc{} || p.ptr != s.data()+s.size()) throw std::runtime_error("invalid unsigned integer");
    return v;
}
bool same_path(const fs::path& a, const fs::path& b) {
    return fs::weakly_canonical(a) == fs::weakly_canonical(b)
        || (fs::exists(a) && fs::exists(b) && fs::equivalent(a,b));
}
}

int main(int argc, char** argv) {
    using namespace cvrp;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") { std::cout << usage; return 0; }
        if (argc < 3) throw std::runtime_error(usage);
        const std::string_view cmd(argv[1]);
        if (cmd == "validate") {
            if (argc != 4 && argc != 6) throw std::runtime_error(usage);
            IdMode mode = IdMode::original;
            if (argc == 6) {
                if (std::string_view(argv[4]) != "--ids") throw std::runtime_error(usage);
                const std::string_view ids(argv[5]);
                if (ids == "cvrplib-x") mode = IdMode::cvrplib_x;
                else if (ids != "original") throw std::runtime_error("unsupported ID mode");
            }
            auto vin = open_input(argv[2]);
            const auto inst = read_instance(vin);
            auto sin = open_input(argv[3]);
            const auto sol = validate(sin, inst, mode);
            std::cout << "VALID cost=" << sol.cost << " routes=" << sol.routes.size() << '\n';
        } else if (cmd == "solve") {
            if (argc < 7 || (argc - 3) % 2 != 0) throw std::runtime_error(usage);
            std::string_view method;
            const char* dest = nullptr;
            const char* tour = nullptr;
            const char* log = nullptr;
            SearchConfig cfg;
            bool has_search_options = false;
            double budget = 0;
            std::set<std::string_view> used;
            for (int i = 3; i < argc; i += 2) {
                const std::string_view k(argv[i]);
                if (!used.insert(k).second || !argv[i+1][0]) throw std::runtime_error("empty or repeated option");
                if (k == "--method") method = argv[i+1];
                else if (k == "--out") dest = argv[i+1];
                else if (k == "--tour") tour = argv[i+1];
                else {
                    has_search_options = true;
                    if (k == "--log") log = argv[i+1];
                    else if (k == "--steps") cfg.steps = count_arg(argv[i+1]);
                    else if (k == "--seed") cfg.seed = count_arg(argv[i+1]);
                    else if (k == "--ls-cap") cfg.ls_cap = count_arg(argv[i+1]);
                    else if (k == "--population" || k == "--neighbors") {
                        const auto n = count_arg(argv[i+1]);
                        if (n == 0 || n > 3000) throw std::runtime_error("population/neighbors outside supported range");
                        if (k == "--population") cfg.population = static_cast<std::size_t>(n);
                        else cfg.neighbors = static_cast<std::size_t>(n);
                    } else if (k == "--cpu-seconds") {
                        std::size_t end = 0;
                        const std::string value(argv[i+1]);
                        budget = std::stod(value, &end);
                        if (end != value.size() || !std::isfinite(budget) || budget <= 0)
                            throw std::runtime_error("invalid CPU budget");
                    } else throw std::runtime_error("unknown option: " + std::string(k));
                }
            }
            const bool stochastic = method == "ga" || method == "multistart" || method == "ma";
            if (!dest || (!stochastic && (has_search_options || (method != "savings" && method != "split"))))
                throw std::runtime_error(usage);
            if (budget > 0 && !used.count("--steps")) cfg.steps = std::numeric_limits<std::uint64_t>::max();
            if ((method == "split") != static_cast<bool>(tour)) throw std::runtime_error("specify --tour only and always for split");
            if (same_path(dest,argv[2]) || (log && (same_path(log,argv[2]) || same_path(log,dest))))
                throw std::runtime_error("instance, solution and log paths must be distinct");
            auto vin = open_input(argv[2]);
            auto inst = read_instance(vin);
            const auto sequence = tour ? parse_tour(tour, inst) : Route{};
            const auto start = stochastic ? process_cpu_seconds() : 0;
            inst.prepare_distances();
            Solution sol;
            if (stochastic) {
                struct Event { std::uint64_t step; double t; Cost cost; std::size_t routes; };
                std::vector<Event> events;
                const auto elapsed = [start] { return process_cpu_seconds() - start; };
                auto result = search(inst, std::string(method), cfg, [&](std::uint64_t step, const Solution& s) {
                    if (log) events.push_back({step, elapsed(), s.cost, s.routes.size()});
                }, [&] { return budget > 0 && elapsed() >= budget; });
                const auto cpu = elapsed();
                if (log) {
                    std::ofstream csv(log);
                    csv << "step,cpu_seconds,cost,routes\n" << std::fixed << std::setprecision(9);
                    for (const auto& e : events) csv << e.step << ',' << e.t << ',' << e.cost << ',' << e.routes << '\n';
                    csv.close();
                    if (!csv) throw std::runtime_error("cannot write incumbent log");
                }
                if (result.best.routes.empty()) throw std::runtime_error("no feasible incumbent within budget");
                sol = std::move(result.best);
                std::cout << "cpu_seconds=" << cpu << " steps=" << result.steps
                          << " evaluations=" << result.evaluations << " seed=" << cfg.seed << '\n';
            } else sol = method == "split" ? split(inst, sequence) : savings(inst);
            std::ofstream out(dest);
            if (!out) throw std::runtime_error(std::string("cannot open output: ") + dest);
            write_solution(out, inst, sol);
            out.close();
            if (!out) throw std::runtime_error("failed to close output");
            std::cout << "cost=" << sol.cost << " routes=" << sol.routes.size() << '\n';
        } else throw std::runtime_error(usage);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
