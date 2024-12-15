#include "CLI11.hpp"
#include "VariadicTable.hpp"
#include "dist.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <string>

using pprinter =
    VariadicTable<std::string, std::string, double, double, double, double>;

#define OMP_PARALLEL_FOR _Pragma("omp parallel for")
#define L2_CACHE 96 * 1024 * 1024
#define L3_CACHE 90 * 1024 * 1024

double calc_data_size(uint64_t N1, uint64_t N2, uint64_t M) {
  return ((double)(N1 * M * sizeof(bf16)) + (double)(N2 * M * sizeof(bf16))) /
         ((double)(2 << 19));
}

class Benchmark {
public:
  dnnl::engine engine;
  dnnl::stream stream;
  pprinter *pt;
  bool debug;
  std::vector<std::string> headers = {
      "Mode",       "N1 / N2 / M",   "Data size (MiB)",
      "Total FLOP", "Duration (ns)", "GFLOPS"};

  Benchmark(dnnl::engine engine, dnnl::stream stream)
      : engine(engine), stream(stream) {
    pt = new pprinter(headers);
  }

  void print_results() {
    pt->print(std::cout);
    pt = new pprinter(headers);
  }

  void run_ip(uint64_t N1, uint64_t N2, uint64_t M) {
    std::vector<bf16> mat_a(N1 * M);
    std::vector<bf16> mat_b(N2 * M);

    std::mt19937_64 rng;
    rng.seed(47);
    std::uniform_real_distribution<float> distrib;

    OMP_PARALLEL_FOR
    for (uint64_t i = 0; i < N1; i++) {
      for (uint64_t j = 0; j < M; j++) {
        mat_a[i * M + j] = (bf16)distrib(rng);
      }
    }

    OMP_PARALLEL_FOR
    for (uint64_t i = 0; i < N2; i++) {
      for (uint64_t j = 0; j < M; j++) {
        mat_b[i * M + j] = (bf16)distrib(rng);
      }
    }

    double data_size = calc_data_size(N1, N2, M);
    uint64_t total_flop = (N1 * N2) * (2 * M - 1);
    std::string dims =
        std::to_string(N1) + "/" + std::to_string(N2) + "/" + std::to_string(M);
    {
      int64_t dur = amx_inner_product(
        N1, N2, M, mat_a.data(), mat_b.data(), engine, stream, debug);
      double gflops =
          ((double)(total_flop)) / ((double)(dur));
      pt->addRow("IP / AMX", dims, data_size, total_flop, dur, gflops);
    }
  }

  void run_gemm(uint64_t N1, uint64_t N2, uint64_t M) {
    std::vector<bf16> mat_a(N1 * M);
    std::vector<bf16> mat_b(M * N2);

    std::mt19937 rng;
    rng.seed(47);
    std::uniform_real_distribution<float> distrib;

    OMP_PARALLEL_FOR
    for (uint64_t i = 0; i < N1; i++) {
      for (uint64_t j = 0; j < M; j++) {
        mat_a[i * M + j] = (bf16)distrib(rng);
      }
    }

    OMP_PARALLEL_FOR
    for (uint64_t i = 0; i < M; i++) {
      for (uint64_t j = 0; j < N2; j++) {
        mat_b[i * N2 + j] = (bf16)distrib(rng);
      }
    }

    double data_size = calc_data_size(N1, N2, M);
    uint64_t total_flop = (N1 * N2) * (2 * M - 1);
    std::string dims =
        std::to_string(N1) + "/" + std::to_string(N2) + "/" + std::to_string(M);

    {
      int64_t dur = amx_matmul(
        N1, N2, M, mat_a.data(), mat_b.data(), engine, stream, debug);
      double gflops =
          ((double)(total_flop)) / ((double)(dur));
      pt->addRow("GEMM / AMX", dims, data_size, total_flop, dur, gflops);
    }
  }
};

void run_bench_sq_matrix() {
  dnnl::engine engine(dnnl::engine::kind::cpu, 0);
  dnnl::stream stream(engine);

  Benchmark bench(engine, stream);

  // Just bench AMX
  std::vector<uint64_t> sizes = {64,   128,  256,  512,   1024,
                                 2048, 4096, 8192, 16384, 32768};
  std::for_each(sizes.begin(), sizes.end(), [&](uint64_t size) {
    bench.run_ip(size, size, size);
  });
  bench.print_results();
  std::for_each(sizes.begin(), sizes.end(), [&](uint64_t size) {
    bench.run_gemm(size, size, size);
  });
  bench.print_results();
}

void run_bench_rect_matrix() {
  dnnl::engine engine(dnnl::engine::kind::cpu, 0);
  dnnl::stream stream(engine);

  Benchmark bench(engine, stream);

  uint64_t const n2_base = 1024 * 1024;
  uint64_t const m = 1024;

  // Just bench AMX
  std::vector<uint64_t> n1s = {32, 64,   128,  256,   512,   1024, 2048,
                               4096, 8192, 16384, 32768};
  std::vector<uint64_t> n2_multipliers = {1, 2, 4, 8};
  std::for_each(n1s.begin(), n1s.end(), [&](uint64_t n1) {
    std::for_each(n2_multipliers.begin(), n2_multipliers.end(),
                  [&](uint64_t n2_multiplier) {
                    uint64_t n2 = n2_base * n2_multiplier;
                    bench.run_ip(n1, n2, m);
                  });
  });
  bench.print_results();
}

int main(int argc, char **argv) {
  CLI::App app{"Intel AMX Benchmark"};
  argv = app.ensure_utf8(argv);

  bool debug = 0;
  app.add_option("-d,--debug", debug, "Enable debug mode");

  CLI11_PARSE(app, argc, argv);

  run_bench_sq_matrix();
  run_bench_rect_matrix();
}
