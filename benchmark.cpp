#include <cstdint>
#include <cstddef>
#include <array>
#include <benchmark/benchmark.h>

// --- Callback Signature ---
using MsgCallback = void(*)(const uint8_t* payload, uint16_t len);

// --- Handler Generator ---
template <uint16_t ID>
void handler_impl(const uint8_t* payload, uint16_t /*len*/) {
    // Volatile write to prevent dead-code elimination
    volatile uint8_t sink = payload[0];
    (void)sink;
}

// --- Data Generation ---
// Generate arrays of unique 16-bit IDs where:
// bits [15:11] = subsystem (0-31)
// bits [10:0] = message type (0-2047)

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

// Dataset sizes
constexpr auto IDS_128 = generate_ids<128>();
constexpr auto IDS_500 = generate_ids<500>();
constexpr auto IDS_2000 = generate_ids<2000>();

// Helper to get callback array from ID array
template <size_t N, size_t... Is>
constexpr std::array<MsgCallback, N> generate_callbacks_impl(std::index_sequence<Is...>) {
    return { handler_impl<Is>... };
}

template <size_t N>
constexpr std::array<MsgCallback, N> generate_callbacks() {
    return generate_callbacks_impl<N>(std::make_index_sequence<N>{});
}

constexpr auto CBS_128 = generate_callbacks<128>();
constexpr auto CBS_500 = generate_callbacks<500>();
constexpr auto CBS_2000 = generate_callbacks<2000>();


// ============================================================================
// Approach 1: compile-time perfect hash over a static array (CHD Algorithm)
// ============================================================================

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

constexpr auto PH_128  = build_perfect_hash(IDS_128, CBS_128);
constexpr auto PH_500  = build_perfect_hash(IDS_500, CBS_500);
constexpr auto PH_2000 = build_perfect_hash(IDS_2000, CBS_2000);


// ============================================================================
// Approach 2: sorted array with constexpr binary search
// ============================================================================

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
    T tmp = a;
    a = b;
    b = tmp;
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
    if constexpr (N > 0) {
        cx_quicksort(arr, 0, N - 1);
    }
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

constexpr auto SA_128  = build_sorted_array(IDS_128, CBS_128);
constexpr auto SA_500  = build_sorted_array(IDS_500, CBS_500);
constexpr auto SA_2000 = build_sorted_array(IDS_2000, CBS_2000);

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


// ============================================================================
// Approach 3: multi-level static array exploiting ID bit structure
// ============================================================================

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
    for (size_t i = 0; i < N; ++i) {
        t.map(keys[i], cbs[i]);
    }
    return t;
}

constexpr auto TRIE_128  = build_trie(IDS_128, CBS_128);
constexpr auto TRIE_500  = build_trie(IDS_500, CBS_500);
constexpr auto TRIE_2000 = build_trie(IDS_2000, CBS_2000);


// ============================================================================
// Benchmarking
// ============================================================================

#include <vector>
#include <random>

// Runtime array of 10,000 randomized IDs (valid and invalid mix)
std::vector<uint16_t> generate_test_data() {
    std::vector<uint16_t> test_ids;
    test_ids.reserve(10000);
    std::mt19937 gen(42);
    std::uniform_int_distribution<uint16_t> dist(0, 65535);

    for (int i = 0; i < 10000; ++i) {
        test_ids.push_back(dist(gen));
    }
    return test_ids;
}

const std::vector<uint16_t> TEST_IDS = generate_test_data();
const uint8_t DUMMY_PAYLOAD[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// ----------------------------------------------------------------------------
// Perfect Hash Benchmarks
// ----------------------------------------------------------------------------

static void BM_PerfectHash_128(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = PH_128.dispatch(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_PerfectHash_128);

static void BM_PerfectHash_500(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = PH_500.dispatch(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_PerfectHash_500);

static void BM_PerfectHash_2000(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = PH_2000.dispatch(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_PerfectHash_2000);

// ----------------------------------------------------------------------------
// Sorted Array Benchmarks
// ----------------------------------------------------------------------------

static void BM_SortedArray_128(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = sa_dispatch(SA_128, id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_SortedArray_128);

static void BM_SortedArray_500(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = sa_dispatch(SA_500, id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_SortedArray_500);

static void BM_SortedArray_2000(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = sa_dispatch(SA_2000, id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_SortedArray_2000);

// ----------------------------------------------------------------------------
// Structured Trie Benchmarks
// ----------------------------------------------------------------------------

static void BM_StructuredTrie_128(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = TRIE_128.lookup(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_StructuredTrie_128);

static void BM_StructuredTrie_500(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = TRIE_500.lookup(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_StructuredTrie_500);

static void BM_StructuredTrie_2000(benchmark::State& state) {
    for (auto _ : state) {
        for (uint16_t id : TEST_IDS) {
            auto handler = TRIE_2000.lookup(id);
            benchmark::DoNotOptimize(handler);
            if (handler) handler(DUMMY_PAYLOAD, sizeof(DUMMY_PAYLOAD));
        }
    }
}
BENCHMARK(BM_StructuredTrie_2000);

BENCHMARK_MAIN();
