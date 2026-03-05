#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>
#include <random>
#include <benchmark/benchmark.h>

using MsgCallback = void(*)(const uint8_t* payload, uint16_t len);

template <uint16_t ID>
void handler_impl(const uint8_t* payload, uint16_t /*len*/) {
    volatile uint8_t sink = payload[0];
    (void)sink;
}

template <size_t N>
constexpr std::array<uint16_t, N> generate_ids() {
    std::array<uint16_t, N> arr{};
    for (size_t i = 0; i < N; ++i) {
        uint16_t sub = (i % 32);
        uint16_t msg = (i / 32) % 2048;
        arr[i] = (sub << 11) | msg;
    }
    return arr;
}

template <size_t N, size_t... Is>
constexpr std::array<MsgCallback, N> generate_callbacks_impl(std::index_sequence<Is...>) {
    return { handler_impl<Is>... };
}

template <size_t N>
constexpr std::array<MsgCallback, N> generate_callbacks() {
    return generate_callbacks_impl<N>(std::make_index_sequence<N>{});
}

constexpr uint32_t mph_hash(uint32_t key, uint32_t seed) {
    key ^= seed;
    key ^= key >> 16;
    key *= 0x45d9f3bU;
    key ^= key >> 16;
    key *= 0x45d9f3bU;
    key ^= key >> 16;
    return key;
}

template <size_t N>
struct PerfectHashTable {
    std::array<int32_t, N> G{};
    std::array<uint32_t, N> keys{};
    std::array<MsgCallback, N> callbacks{};

    constexpr uint32_t lookup_index(uint32_t key) const {
        uint32_t bucket = mph_hash(key, 0) % N;
        int32_t d = G[bucket];
        if (d < 0) {
            return static_cast<uint32_t>(-d - 1);
        }
        return mph_hash(key, static_cast<uint32_t>(d)) % N;
    }

    inline MsgCallback dispatch(uint32_t msg_id) const {
        uint32_t slot = lookup_index(msg_id);
        if (slot < N && keys[slot] == msg_id) {
            return callbacks[slot];
        }
        return nullptr;
    }
};

template <size_t N>
constexpr PerfectHashTable<N> build_perfect_hash(
    const std::array<uint16_t, N>& input_keys,
    const std::array<MsgCallback, N>& input_callbacks)
{
    PerfectHashTable<N> table{};
    std::array<bool, N> slot_used{};
    for (auto& s : slot_used) s = false;

    struct Bucket {
        std::array<uint32_t, N> members{};
        std::array<MsgCallback, N> callbacks{};
        size_t count = 0;
    };
    std::array<Bucket, N> buckets{};

    for (size_t i = 0; i < N; ++i) {
        uint32_t b = mph_hash(input_keys[i], 0) % N;
        buckets[b].members[buckets[b].count] = input_keys[i];
        buckets[b].callbacks[buckets[b].count] = input_callbacks[i];
        buckets[b].count++;
    }

    std::array<size_t, N> bucket_order{};
    for (size_t i = 0; i < N; ++i) bucket_order[i] = i;
    for (size_t i = 0; i < N; ++i) {
        size_t max_idx = i;
        for (size_t j = i + 1; j < N; ++j) {
            if (buckets[bucket_order[j]].count >
                buckets[bucket_order[max_idx]].count)
                max_idx = j;
        }
        auto tmp = bucket_order[i];
        bucket_order[i] = bucket_order[max_idx];
        bucket_order[max_idx] = tmp;
    }

    for (size_t bi = 0; bi < N; ++bi) {
        size_t idx = bucket_order[bi];
        const auto& bkt = buckets[idx];
        if (bkt.count <= 1) break;

        for (uint32_t d = 1; d < N * 10; ++d) {
            std::array<uint32_t, N> trial_slots{};
            size_t placed = 0;
            bool collision = false;

            for (size_t k = 0; k < bkt.count; ++k) {
                uint32_t slot = mph_hash(bkt.members[k], d) % N;
                if (slot_used[slot]) { collision = true; break; }
                for (size_t p = 0; p < placed; ++p) {
                    if (trial_slots[p] == slot) {
                        collision = true; break;
                    }
                }
                if (collision) break;
                trial_slots[placed++] = slot;
            }

            if (!collision && placed == bkt.count) {
                table.G[idx] = static_cast<int32_t>(d);
                for (size_t k = 0; k < bkt.count; ++k) {
                    uint32_t slot = mph_hash(bkt.members[k], d) % N;
                    slot_used[slot] = true;
                    table.keys[slot] = bkt.members[k];
                    table.callbacks[slot] = bkt.callbacks[k];
                }
                break;
            }
        }
    }

    size_t free_cursor = 0;
    auto next_free = [&]() -> size_t {
        while (free_cursor < N && slot_used[free_cursor]) ++free_cursor;
        return free_cursor;
    };

    for (size_t bi = 0; bi < N; ++bi) {
        size_t idx = bucket_order[bi];
        const auto& bkt = buckets[idx];
        if (bkt.count == 1) {
            size_t slot = next_free();
            slot_used[slot] = true;
            table.keys[slot] = bkt.members[0];
            table.callbacks[slot] = bkt.callbacks[0];
            table.G[idx] = -static_cast<int32_t>(slot) - 1;
        }
    }

    return table;
}

struct CallbackEntry {
    uint16_t msg_id;
    MsgCallback handler;

    constexpr bool operator<(const CallbackEntry& other) const {
        return msg_id < other.msg_id;
    }
    constexpr bool operator>(const CallbackEntry& other) const {
        return msg_id > other.msg_id;
    }
};

template<typename T>
constexpr void cx_swap(T& a, T& b) {
    T tmp = a; a = b; b = tmp;
}

template<typename T, size_t N>
constexpr void cx_quicksort(std::array<T, N>& arr, int low, int high) {
    if (low < high) {
        T pivot = arr[low + (high - low) / 2];
        int i = low;
        int j = high;
        while (i <= j) {
            while (arr[i] < pivot) i++;
            while (arr[j] > pivot) j--;
            if (i <= j) {
                cx_swap(arr[i], arr[j]);
                i++;
                j--;
            }
        }
        if (low < j) cx_quicksort(arr, low, j);
        if (i < high) cx_quicksort(arr, i, high);
    }
}

template<size_t N>
constexpr std::array<CallbackEntry, N> cx_sort(std::array<CallbackEntry, N> arr) {
    if constexpr (N > 0) cx_quicksort(arr, 0, N - 1);
    return arr;
}

template <size_t N>
constexpr std::array<CallbackEntry, N> build_sorted_array(
    const std::array<uint16_t, N>& keys,
    const std::array<MsgCallback, N>& cbs)
{
    std::array<CallbackEntry, N> arr{};
    for (size_t i = 0; i < N; ++i) {
        arr[i].msg_id = keys[i];
        arr[i].handler = cbs[i];
    }
    return cx_sort(arr);
}

template <size_t N>
inline MsgCallback sa_dispatch(const std::array<CallbackEntry, N>& table, uint16_t msg_id) {
    int lo = 0;
    int hi = N - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (table[mid].msg_id < msg_id) {
            lo = mid + 1;
        } else if (table[mid].msg_id > msg_id) {
            hi = mid - 1;
        } else {
            return table[mid].handler;
        }
    }
    return nullptr;
}

static constexpr size_t NUM_SUBSYSTEMS = 32;
static constexpr size_t NUM_MSG_TYPES  = 2048;

constexpr uint8_t get_subsystem(uint16_t id) {
    return static_cast<uint8_t>((id >> 11) & 0x1F);
}
constexpr uint16_t get_msg_type(uint16_t id) {
    return static_cast<uint16_t>(id & 0x07FF);
}

struct TrieDispatchTable {
    std::array<std::array<MsgCallback, NUM_MSG_TYPES>, NUM_SUBSYSTEMS> slots{};

    constexpr TrieDispatchTable() : slots{} {
        for (auto& row : slots)
            for (auto& cell : row)
                cell = nullptr;
    }

    constexpr void map(uint16_t msg_id, MsgCallback handler) {
        slots[get_subsystem(msg_id)][get_msg_type(msg_id)] = handler;
    }

    constexpr MsgCallback lookup(uint16_t msg_id) const {
        return slots[get_subsystem(msg_id)][get_msg_type(msg_id)];
    }
};

template <size_t N>
inline constexpr TrieDispatchTable build_trie(
    const std::array<uint16_t, N>& keys,
    const std::array<MsgCallback, N>& cbs)
{
    TrieDispatchTable t;
    for (size_t i = 0; i < N; ++i) t.map(keys[i], cbs[i]);
    return t;
}

std::vector<uint16_t> generate_test_data() {
    std::vector<uint16_t> test_ids;
    test_ids.reserve(10000);
    std::mt19937 gen(42);
    std::uniform_int_distribution<uint16_t> dist(0, 65535);
    for (int i = 0; i < 10000; ++i) test_ids.push_back(dist(gen));
    return test_ids;
}

const std::vector<uint16_t> TEST_IDS = generate_test_data();
const uint8_t DUMMY_PAYLOAD[8] = {1, 2, 3, 4, 5, 6, 7, 8};

template <size_t N>
struct Tables {
    static constexpr auto IDS = generate_ids<N>();
    static constexpr auto CBS = generate_callbacks<N>();
    static constexpr auto PH = build_perfect_hash(IDS, CBS);
    static constexpr auto SA = build_sorted_array(IDS, CBS);
    static constexpr auto TRIE = build_trie(IDS, CBS);
};

constexpr size_t cx_pow(size_t base, size_t exp) {
    size_t res = 1;
    for (size_t i = 0; i < exp; ++i) res *= base;
    return res;
}

template <size_t Lower, size_t Mult, size_t... Is>
constexpr auto make_geom_seq(std::index_sequence<Is...>) {
    return std::index_sequence<(Lower * cx_pow(Mult, Is))...>{};
}

template <size_t... Is, size_t... Js>
constexpr auto merge_seqs(std::index_sequence<Is...>, std::index_sequence<Js...>) {
    return std::index_sequence<Is..., Js...>{};
}

using Hardcoded_checks = std::index_sequence<4, 126, 13056>;
using CustomRange = decltype(make_geom_seq<8, 8>(std::make_index_sequence<4>{}));
using AllBenchmarkSizes = decltype(merge_seqs(Hardcoded_checks{}, CustomRange{}));

template <size_t N>
void BM_PerfectHash(benchmark::State& state) {
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
    (benchmark::RegisterBenchmark(std::string("PerfectHash/" + std::to_string(Ns)), BM_PerfectHash<Ns>), ...);
    (benchmark::RegisterBenchmark(std::string("SortedArray/" + std::to_string(Ns)), BM_SortedArray<Ns>), ...);
    (benchmark::RegisterBenchmark(std::string("StructuredTrie/" + std::to_string(Ns)), BM_StructuredTrie<Ns>), ...);
}

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    RegisterAll(AllBenchmarkSizes{});
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}