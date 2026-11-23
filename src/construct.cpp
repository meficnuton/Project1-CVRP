// Xây dựng nghiệm: Split (DP tiền tố trên DAG cắt) và Parallel Clarke–Wright Savings.
// Toán học theo docs/project-i/01-specification.md (§Split dùng DAG, §Toán tử).
//
// An toàn số học: mọi tổng (prefix nhu cầu, chi phí trong tuyến, chi phí nghiệm,
// tiết kiệm, tải gộp) đều qua helper kiểm tra tràn số nguyên có dấu 64-bit;
// tràn -> std::runtime_error, không wrap im lặng. Trong miền khai báo
// (DIMENSION<=3000, |tọa độ|<=1e6, Q<=1e12) các giá trị này nằm gọn trong int64,
// các helper chỉ là lớp phòng thủ cho dữ liệu ngoài hợp đồng.

#include "cvrp.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace cvrp {
namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

Cost add_checked(Cost a, Cost b, const char* ctx) {
    constexpr auto lo = std::numeric_limits<Cost>::min();
    constexpr auto hi = std::numeric_limits<Cost>::max();
    if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b))
        fail(std::string(ctx) + ": signed 64-bit arithmetic overflow");
    return a + b;
}

Cost sub_checked(Cost a, Cost b, const char* ctx) {
    constexpr auto lo = std::numeric_limits<Cost>::min();
    constexpr auto hi = std::numeric_limits<Cost>::max();
    if ((b > 0 && a < lo + b) || (b < 0 && a > hi + b))
        fail(std::string(ctx) + ": signed 64-bit arithmetic overflow");
    return a - b;
}

// Ma trận khoảng cách row-major chỉ có trên đường solver; dùng sớm thì phải chuẩn bị.
void require_distances(const Instance& inst, const char* ctx) {
    const std::size_t n_total = inst.nodes.size();
    if (inst.distances.size() != n_total * n_total)
        fail(std::string(ctx) +
             ": distance matrix not prepared (call Instance::prepare_distances first)");
}

// Chi phí tuyến khách r (không có kho trong r, ngầm nối cả hai đầu về kho).
Cost route_cost(const Instance& inst, const Route& r, const char* ctx) {
    Cost c = inst.dist(0, r.front());
    for (std::size_t k = 0; k + 1 < r.size(); ++k)
        c = add_checked(c, inst.dist(r[k], r[k + 1]), ctx);
    return add_checked(c, inst.dist(r.back(), 0), ctx);
}

} // namespace

// Split tối ưu phép cắt trên hoán vị π cho trước (không tối ưu thứ tự khách).
// Đường đi 0->n trên DAG cung (i,j), 0<=i<j<=n, tồn tại khi P[j]-P[i]<=Q;
// trọng số cung = c(0,pi_{i+1}) + H[j]-H[i+1] + c(pi_j,0). DP O(n^2) xấu nhất,
// O(n) bộ nhớ phụ ngoài dữ liệu instance.
Solution split(const Instance& inst, const Route& tour) {
    if (inst.nodes.empty()) fail("split: instance has no depot node");
    const std::size_t m = inst.nodes.size() - 1; // số khách
    if (tour.size() != m)
        fail("split: tour is not a permutation of the customer set (wrong length)");

    // π phải là hoán vị đúng: mỗi khách 1..m xuất hiện đúng một lần, không có kho.
    std::vector<char> seen(m + 1, 0);
    for (Node v : tour) {
        if (v == 0 || v > m || seen[v])
            fail("split: tour is not a permutation of the customer set (bad or repeated customer)");
        seen[v] = 1;
    }

    Solution sol;
    if (m == 0) return sol; // không có khách: hoán vị rỗng, nghiệm rỗng

    require_distances(inst, "split");
    if (inst.capacity <= 0) fail("split: instance capacity must be positive");
    const Cost Q = inst.capacity;

    // P[j] = tổng nhu cầu pi_1..pi_j ; H[j] = chi phí đi trong pi_1..pi_j (H[1]=0).
    // π_k (đánh số từ 1) = tour[k-1].
    std::vector<Cost> P(m + 1, 0), H(m + 1, 0);
    for (std::size_t j = 1; j <= m; ++j)
        P[j] = add_checked(P[j - 1], inst.nodes[tour[j - 1]].demand, "split demand prefix");
    for (std::size_t j = 2; j <= m; ++j)
        H[j] = add_checked(H[j - 1], inst.dist(tour[j - 2], tour[j - 1]), "split internal travel prefix");

    const Cost INF = std::numeric_limits<Cost>::max();
    std::vector<Cost> F(m + 1, INF);
    std::vector<Node> pred(m + 1, 0);
    F[0] = 0;

    for (std::size_t i = 0; i < m; ++i) {
        if (F[i] == INF) continue; // không tới được i, mọi cung xuất phát từ i vô nghĩa
        for (std::size_t j = i + 1; j <= m; ++j) {
            // Nhu cầu không âm nên P không giảm: cửa sổ chỉ nới ra, được phép break.
            if (sub_checked(P[j], P[i], "split demand window") > Q) break;
            // Cung i -> j phục vụ pi_{i+1}..pi_j (= tour[i..j-1]).
            Cost z = F[i];
            z = add_checked(z, inst.dist(0, tour[i]), "split arc cost");               // kho -> pi_{i+1}
            z = add_checked(z, sub_checked(H[j], H[i + 1], "split arc cost"), "split arc cost");
            z = add_checked(z, inst.dist(tour[j - 1], 0), "split arc cost");           // pi_j -> kho
            if (z < F[j]) { // so sánh ngặt: khi bằng giá trị, giữ tiền nhiệm xuất hiện trước
                F[j] = z;
                pred[j] = static_cast<Node>(i);
            }
        }
    }

    if (F[m] == INF) fail("split: no feasible partition of the tour under capacity Q");
    sol.cost = F[m];

    // Truy vết pred từ n về 0, thu được các đoạn từ phải sang trái, rồi đảo lại.
    for (std::size_t j = m; j > 0;) {
        const std::size_t i = pred[j];
        sol.routes.emplace_back(tour.begin() + static_cast<std::ptrdiff_t>(i),
                                tour.begin() + static_cast<std::ptrdiff_t>(j));
        j = i;
    }
    std::reverse(sol.routes.begin(), sol.routes.end());
    return sol;
}

// Parallel Clarke–Wright: mỗi khách một tuyến; duyệt tiết kiệm s(i,j) > 0 giảm dần,
// hòa theo ID gốc tăng dần khi có hòa; chỉ nối hai tuyến khác nhau khi i và j nằm
// ở đầu mút của tuyến mình và tổng tải <= Q; đảo chiều tuyến khi cần (đối xứng).
Solution savings(const Instance& inst) {
    if (inst.nodes.empty()) fail("savings: instance has no depot node");
    if (inst.capacity <= 0) fail("savings: instance capacity must be positive");
    const std::size_t m = inst.nodes.size() - 1;
    Solution sol;
    if (m == 0) return sol;

    require_distances(inst, "savings");
    const std::vector<Customer>& nodes = inst.nodes;
    const Cost Q = inst.capacity;

    std::vector<Route> routes(m);
    std::vector<std::size_t> route_of(m + 1, 0); // khách (chỉ số nội bộ) -> chỉ số tuyến
    std::vector<Cost> load(m, 0);
    for (std::size_t k = 0; k < m; ++k) {
        routes[k] = Route{static_cast<Node>(k + 1)};
        route_of[k + 1] = k;
        load[k] = nodes[k + 1].demand;
    }

    // s(x,y) = c(0,x) + c(0,y) - c(x,y); chỉ giữ s > 0. Cặp lưu đúng một lần, x < y.
    struct Cand { Cost s; Node x, y; };
    std::vector<Cand> cands;
    cands.reserve((m * (m - 1)) / 2);
    for (Node x = 1; x <= m; ++x) {
        const Cost dx = inst.dist(0, x);
        for (Node y = x + 1; y <= m; ++y) {
            Cost s = add_checked(dx, inst.dist(0, y), "savings value");
            s = sub_checked(s, inst.dist(x, y), "savings value");
            if (s > 0) cands.push_back(Cand{s, x, y});
        }
    }

    // Tiết kiệm giảm dần; hòa theo ID gốc tăng dần (cặp ID nhỏ rồi ID lớn).
    const auto by_saving = [&nodes](const Cand& a, const Cand& b) {
        if (a.s != b.s) return a.s > b.s;
        const Id a1 = std::min(nodes[a.x].id, nodes[a.y].id);
        const Id a2 = std::max(nodes[a.x].id, nodes[a.y].id);
        const Id b1 = std::min(nodes[b.x].id, nodes[b.y].id);
        const Id b2 = std::max(nodes[b.x].id, nodes[b.y].id);
        if (a1 != b1) return a1 < b1;
        return a2 < b2;
    };
    std::sort(cands.begin(), cands.end(), by_saving);

    for (const Cand& c : cands) {
        const Node x = c.x, y = c.y;
        const std::size_t ra = route_of[x], rb = route_of[y];
        if (ra == rb) continue; // cùng tuyến
        const Route& Ra = routes[ra];
        const Route& Rb = routes[rb];
        const bool x_front = Ra.front() == x, x_back = Ra.back() == x;
        const bool y_front = Rb.front() == y, y_back = Rb.back() == y;
        // Khách nằm giữa tuyến thì không nối được.
        if ((!x_front && !x_back) || (!y_front && !y_back)) continue;
        const Cost merged_load = add_checked(load[ra], load[rb], "savings merge load");
        if (merged_load > Q) continue;

        // Nối theo hướng tạo cạnh (x,y) tại điểm nối, đảo chiều tuyến khi cần.
        // Với tuyến một khách, front == back; thứ tự các nhánh cố định hướng nối.
        Route merged;
        if (x_back && y_front) {
            merged = Ra;
            merged.insert(merged.end(), Rb.begin(), Rb.end());
        } else if (x_front && y_back) {
            merged = Rb;
            merged.insert(merged.end(), Ra.begin(), Ra.end());
        } else if (x_front && y_front) {
            merged = Route(Ra.rbegin(), Ra.rend());
            merged.insert(merged.end(), Rb.begin(), Rb.end());
        } else { // x_back && y_back
            merged = Ra;
            merged.insert(merged.end(), Rb.rbegin(), Rb.rend());
        }

        // Làm mới chủ sở hữu của mọi khách trong tuyến gộp; tuyến rb thành rỗng.
        load[ra] = merged_load;
        load[rb] = 0;
        routes[ra] = std::move(merged);
        routes[rb].clear();
        for (Node v : routes[ra]) route_of[v] = ra;
    }

    // Chỉ giữ tuyến khác rỗng, sắp thứ tự tất định theo ID gốc (so từ điển).
    for (const Route& r : routes)
        if (!r.empty()) sol.routes.push_back(r);
    std::sort(sol.routes.begin(), sol.routes.end(), [&nodes](const Route& a, const Route& b) {
        const std::size_t kmax = std::min(a.size(), b.size());
        for (std::size_t k = 0; k < kmax; ++k) {
            const Id ia = nodes[a[k]].id, ib = nodes[b[k]].id;
            if (ia != ib) return ia < ib;
        }
        return a.size() < b.size();
    });

    Cost total = 0;
    for (const Route& r : sol.routes)
        total = add_checked(total, route_cost(inst, r, "savings cost"), "savings cost");
    sol.cost = total;
    return sol;
}

} // namespace cvrp
