# Project I — CVRP

Nghiên cứu, thiết kế và đánh giá các thuật toán giải bài toán Capacitated Vehicle Routing Problem (CVRP).

## Định hướng

- Nghiên cứu mô hình toán học và các phương pháp giải CVRP.
- Tập trung vào Evolutionary Algorithms và Metaheuristics.
- Triển khai và đánh giá thuật toán bằng thực nghiệm có thể tái lập.

## Cấu trúc hiện tại

- `report/`: báo cáo của dự án theo tuần.
- Trong `report/week*`, chỉ các file `.pdf` được đưa vào Git; các file nguồn và file tạm được bỏ qua bằng `.gitignore`.
- `docs/project-i/`: đặc tả, giao thức, kiến trúc (`architecture.md`) và bàn giao theo từng phiếu.
- `src/`, `tests/`: solver C++17 và kiểm thử CLI bằng Python 3.10+.
- `scripts/`, `data/cvrplib-x/`: script tải & xác thực trọn bộ 100 instance CVRPLib Set X cùng `manifest.csv` (SHA-256, BKS).

## Tải và xác thực bộ dữ liệu CVRPLib Set X

```sh
python scripts/download_cvrplib_x.py --cvrp-exe build/cvrp.exe
```

Script tải đủ 100 cặp `.vrp` / `.sol` BKS của bộ X vào `data/cvrplib-x/`, chạy `build/cvrp.exe validate ... --ids cvrplib-x` cho từng cặp và xuất `data/cvrplib-x/manifest.csv`. Các file thô `.vrp`/`.sol` được bỏ qua trong `.gitignore`; chỉ `manifest.csv` được theo dõi bằng Git.

## Build và kiểm thử solver

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Trên Windows, dùng `build/cvrp.exe`; thay bằng `build/cvrp` trên Linux. `ctest` chạy cả oracle Split duyệt toàn bộ điểm cắt và các ca validator từ chối nghiệm sai.

```sh
build/cvrp.exe solve sample.vrp --method savings --out sample-out.sol
build/cvrp.exe validate sample.vrp sample-out.sol
build/cvrp.exe validate X-n101-k25.vrp X-n101-k25.sol --ids cvrplib-x
```

`--method split --tour 40,5,83,3` nhận một hoán vị theo **ID gốc** của khách trong instance tương ứng. Nghiệm do solver xuất luôn theo ID gốc. `--ids cvrplib-x` chỉ dùng cho file tham chiếu có quy ước nhãn bộ X. Solver hiện chỉ hỗ trợ CVRP đối xứng một kho, `EUC_2D` tọa độ nguyên, làm tròn từng cạnh và số tuyến tự do.

### GA, multi-start và MA

```sh
for method in ga multistart ma; do
  build/cvrp.exe solve examples/line4.vrp --method "$method" --seed 17 --steps 100 --out "build/$method.sol" --log "build/$method.csv"
  build/cvrp.exe validate examples/line4.vrp "build/$method.sol"
done
```

`--steps` là số con hoặc số lần khởi tạo lại, không phải số move; `--ls-cap` là trần candidate trên một nghiệm. Dùng `--cpu-seconds` khi đo theo CPU thay vì so theo số bước. Seed và số bước cố định tái lập trên cùng bản build; cột timestamp vẫn có thể khác.
