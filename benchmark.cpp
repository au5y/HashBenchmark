#include <string>
#include <vector>
#include <random>
#include <benchmark/benchmark.h>
#include "dispatch_tables.hpp"

// ---------------------------------------------------------------------------
// Size sequence configuration
//
// Benchmark sizes are BASE^1, BASE^2, ..., BASE^STEPS.
// Override at build time via CMake options -DBENCH_BASE=N -DBENCH_STEPS=N,
// or directly with compiler flags -DBENCH_BASE=N -DBENCH_STEPS=N.
//
// Defaults: BASE=8, STEPS=3  →  {8, 64, 512}
//
// Larger N values (e.g. STEPS=5 → max 32768) require elevated constexpr
// limits; add to CMakeLists.txt or your compiler invocation:
//   -fconstexpr-depth=100000
//   -fconstexpr-loop-limit=100000000
//   -fconstexpr-ops-limit=1000000000
// ---------------------------------------------------------------------------
#ifndef BENCH_BASE
#  define BENCH_BASE 8
#endif
#ifndef BENCH_STEPS
#  define BENCH_STEPS 3
#endif

static constexpr size_t kBase  = BENCH_BASE;
static constexpr size_t kSteps = BENCH_STEPS;

// Compile-time integer power: Pow<B, E>::value == B^E
template <size_t Base, size_t Exp>
struct Pow : std::integral_constant<size_t, Base * Pow<Base, Exp - 1>::value> {};
template <size_t Base>
struct Pow<Base, 0> : std::integral_constant<size_t, 1> {};

// Build index_sequence<Base^1, Base^2, ..., Base^Steps>
template <size_t Base, size_t Steps, size_t... Vs>
struct ExpSeq : ExpSeq<Base, Steps - 1, Pow<Base, Steps>::value, Vs...> {};
template <size_t Base, size_t... Vs>
struct ExpSeq<Base, 0, Vs...> { using type = std::index_sequence<Vs...>; };

using BenchmarkSizes = typename ExpSeq<kBase, kSteps>::type;

// ---------------------------------------------------------------------------
// Test data: 10k random 16-bit IDs (fixed seed for reproducibility)
// Most lookups are misses, exercising both hit and miss paths.
// ---------------------------------------------------------------------------
static std::vector<uint16_t> generate_test_data() {
    std::vector<uint16_t> ids;
    ids.reserve(10000);
    std::mt19937 gen(42);
    std::uniform_int_distribution<uint16_t> dist(0, 65535);
    for (int i = 0; i < 10000; ++i) ids.push_back(dist(gen));
    return ids;
}

static const std::vector<uint16_t> TEST_IDS    = generate_test_data();
static const uint8_t               DUMMY_PAYLOAD[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// ---------------------------------------------------------------------------
// Benchmark functions
// state.counters["bytes"] reports the in-memory size of each dispatch table.
// ---------------------------------------------------------------------------
template <size_t N>
void BM_PerfectHash(benchmark::State& state) {
    state.counters["bytes"] = sizeof(Tables<N>::PH);
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = Tables<N>::PH.dispatch(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}

template <size_t N>
void BM_SortedArray(benchmark::State& state) {
    state.counters["bytes"] = sizeof(Tables<N>::SA);
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = sa_dispatch(Tables<N>::SA, id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}

template <size_t N>
void BM_StructuredTrie(benchmark::State& state) {
    // Trie size is always NUM_SUBSYSTEMS * NUM_MSG_TYPES * sizeof(pointer),
    // independent of N — the full address space is always allocated.
    state.counters["bytes"] = sizeof(Tables<N>::TRIE);
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = Tables<N>::TRIE.lookup(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}

template <size_t... Ns>
void RegisterAll(std::index_sequence<Ns...>) {
    (benchmark::RegisterBenchmark(
        std::string("PerfectHash/") + std::to_string(Ns), BM_PerfectHash<Ns>), ...);
    (benchmark::RegisterBenchmark(
        std::string("SortedArray/") + std::to_string(Ns), BM_SortedArray<Ns>), ...);
    (benchmark::RegisterBenchmark(
        std::string("StructuredTrie/") + std::to_string(Ns), BM_StructuredTrie<Ns>), ...);
}

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    RegisterAll(BenchmarkSizes{});
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
