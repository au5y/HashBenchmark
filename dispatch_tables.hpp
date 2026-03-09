#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

using MsgCallback = void(*)(const uint8_t* payload, uint16_t len);

// ---------------------------------------------------------------------------
// Test handlers — one instantiation per message ID
// ---------------------------------------------------------------------------
template <uint16_t ID>
void handler_impl(const uint8_t* payload, uint16_t /*len*/) {
    volatile uint8_t sink = payload[0];
    (void)sink;
}

template <size_t N>
constexpr std::array<uint16_t, N> generate_ids() {
    std::array<uint16_t, N> arr{};
    for (size_t i = 0; i < N; ++i) {
        uint16_t sub = static_cast<uint16_t>(i % 32);
        uint16_t msg = static_cast<uint16_t>((i / 32) % 2048);
        arr[i] = static_cast<uint16_t>((sub << 11) | msg);
    }
    return arr;
}

template <size_t N, size_t... Is>
constexpr std::array<MsgCallback, N> generate_callbacks_impl(std::index_sequence<Is...>) {
    return { handler_impl<static_cast<uint16_t>(Is)>... };
}

template <size_t N>
constexpr std::array<MsgCallback, N> generate_callbacks() {
    return generate_callbacks_impl<N>(std::make_index_sequence<N>{});
}

// ---------------------------------------------------------------------------
// Strategy 1: Compile-time minimal perfect hash (CHD / Hanov algorithm)
//   Lookup: O(1) — two hash evaluations + two array reads
// ---------------------------------------------------------------------------
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
    // G[i] > 0  : displacement seed for bucket i
    // G[i] <= 0 : direct slot index (-G[i]-1) for singleton buckets
    std::array<int32_t, N>    G{};
    std::array<uint32_t, N>   keys{};
    std::array<MsgCallback, N> callbacks{};

    constexpr uint32_t lookup_index(uint32_t key) const {
        uint32_t bucket = mph_hash(key, 0) % N;
        int32_t d = G[bucket];
        if (d < 0) return static_cast<uint32_t>(-d - 1);
        return mph_hash(key, static_cast<uint32_t>(d)) % N;
    }

    inline MsgCallback dispatch(uint32_t msg_id) const {
        uint32_t slot = lookup_index(msg_id);
        if (slot < N && keys[slot] == msg_id) return callbacks[slot];
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
        std::array<uint32_t, N>    members{};
        std::array<MsgCallback, N> callbacks{};
        size_t count = 0;
    };
    std::array<Bucket, N> buckets{};

    for (size_t i = 0; i < N; ++i) {
        uint32_t b = mph_hash(input_keys[i], 0) % N;
        buckets[b].members[buckets[b].count]   = input_keys[i];
        buckets[b].callbacks[buckets[b].count] = input_callbacks[i];
        buckets[b].count++;
    }

    // Sort buckets by size descending (selection sort — constexpr safe)
    std::array<size_t, N> bucket_order{};
    for (size_t i = 0; i < N; ++i) bucket_order[i] = i;
    for (size_t i = 0; i < N; ++i) {
        size_t max_idx = i;
        for (size_t j = i + 1; j < N; ++j) {
            if (buckets[bucket_order[j]].count > buckets[bucket_order[max_idx]].count)
                max_idx = j;
        }
        auto tmp = bucket_order[i];
        bucket_order[i] = bucket_order[max_idx];
        bucket_order[max_idx] = tmp;
    }

    // Process multi-key buckets — find displacement value d
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
                    if (trial_slots[p] == slot) { collision = true; break; }
                }
                if (collision) break;
                trial_slots[placed++] = slot;
            }

            if (!collision && placed == bkt.count) {
                table.G[idx] = static_cast<int32_t>(d);
                for (size_t k = 0; k < bkt.count; ++k) {
                    uint32_t slot = mph_hash(bkt.members[k], d) % N;
                    slot_used[slot]        = true;
                    table.keys[slot]       = bkt.members[k];
                    table.callbacks[slot]  = bkt.callbacks[k];
                }
                break;
            }
        }
    }

    // Assign singleton and empty buckets to remaining free slots
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
            table.keys[slot]      = bkt.members[0];
            table.callbacks[slot] = bkt.callbacks[0];
            table.G[idx]          = -static_cast<int32_t>(slot) - 1;
        }
    }

    return table;
}

// ---------------------------------------------------------------------------
// Strategy 2: Sorted array with compile-time quicksort + binary search
//   Lookup: O(log N) — cache-friendly, minimal code
// ---------------------------------------------------------------------------
struct CallbackEntry {
    uint16_t   msg_id;
    MsgCallback handler;

    constexpr bool operator<(const CallbackEntry& o) const { return msg_id < o.msg_id; }
    constexpr bool operator>(const CallbackEntry& o) const { return msg_id > o.msg_id; }
};

template <typename T>
constexpr void cx_swap(T& a, T& b) { T tmp = a; a = b; b = tmp; }

template <typename T, size_t N>
constexpr void cx_quicksort(std::array<T, N>& arr, int low, int high) {
    if (low >= high) return;
    T pivot = arr[low + (high - low) / 2];
    int i = low, j = high;
    while (i <= j) {
        while (arr[i] < pivot) i++;
        while (arr[j] > pivot) j--;
        if (i <= j) { cx_swap(arr[i], arr[j]); i++; j--; }
    }
    if (low < j)  cx_quicksort(arr, low, j);
    if (i < high) cx_quicksort(arr, i, high);
}

template <size_t N>
constexpr std::array<CallbackEntry, N> cx_sort(std::array<CallbackEntry, N> arr) {
    if constexpr (N > 1) cx_quicksort(arr, 0, static_cast<int>(N) - 1);
    return arr;
}

template <size_t N>
constexpr std::array<CallbackEntry, N> build_sorted_array(
    const std::array<uint16_t, N>& keys,
    const std::array<MsgCallback, N>& cbs)
{
    std::array<CallbackEntry, N> arr{};
    for (size_t i = 0; i < N; ++i) { arr[i].msg_id = keys[i]; arr[i].handler = cbs[i]; }
    return cx_sort(arr);
}

template <size_t N>
inline MsgCallback sa_dispatch(const std::array<CallbackEntry, N>& table, uint16_t msg_id) {
    int lo = 0, hi = static_cast<int>(N) - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if      (table[mid].msg_id < msg_id) lo = mid + 1;
        else if (table[mid].msg_id > msg_id) hi = mid - 1;
        else                                 return table[mid].handler;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Strategy 3: Two-level static trie exploiting ID bit-field structure
//   16-bit ID layout: [15:11] subsystem (5 bits), [10:0] message type (11 bits)
//   Lookup: O(1) — two array reads + two bit-shifts, branchless
//   Size: always NUM_SUBSYSTEMS * NUM_MSG_TYPES * sizeof(pointer), independent of N
// ---------------------------------------------------------------------------
static constexpr size_t NUM_SUBSYSTEMS = 32;
static constexpr size_t NUM_MSG_TYPES  = 2048;

constexpr uint8_t  get_subsystem(uint16_t id) { return static_cast<uint8_t>((id >> 11) & 0x1F); }
constexpr uint16_t get_msg_type (uint16_t id) { return static_cast<uint16_t>(id & 0x07FF); }

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

// ---------------------------------------------------------------------------
// Combined holder — benchmark.cpp instantiates Tables<N> for each size
// ---------------------------------------------------------------------------
template <size_t N>
struct Tables {
    static constexpr auto IDS  = generate_ids<N>();
    static constexpr auto CBS  = generate_callbacks<N>();
    static constexpr auto PH   = build_perfect_hash(IDS, CBS);
    static constexpr auto SA   = build_sorted_array(IDS, CBS);
    static constexpr auto TRIE = build_trie(IDS, CBS);
};

// ---------------------------------------------------------------------------
// COMPILE_BENCH mode — used by measure_compile_time.py
//
// To measure compile time for a single (strategy, N) pair, compile a .cpp
// containing:
//
//   #define COMPILE_BENCH
//   #define COMPILE_N  <n>
//   #define STRATEGY_PH    // or STRATEGY_SA / STRATEGY_TRIE
//   #include "dispatch_tables.hpp"
//   int main() { return static_cast<int>(bench_size_bytes); }
//
// bench_size_bytes is constexpr — the compiler must evaluate the full
// constexpr construction to resolve it, making compile time a proxy for
// constexpr evaluation cost.
// ---------------------------------------------------------------------------
#ifdef COMPILE_BENCH
#  ifndef COMPILE_N
#    error "Define COMPILE_N to the table size when using COMPILE_BENCH"
#  endif

static constexpr size_t _bench_N   = COMPILE_N;
static constexpr auto   _bench_IDS = generate_ids<_bench_N>();
static constexpr auto   _bench_CBS = generate_callbacks<_bench_N>();

#  if defined(STRATEGY_PH)
static constexpr auto _bench_table = build_perfect_hash(_bench_IDS, _bench_CBS);
#  elif defined(STRATEGY_SA)
static constexpr auto _bench_table = build_sorted_array(_bench_IDS, _bench_CBS);
#  elif defined(STRATEGY_TRIE)
static constexpr auto _bench_table = build_trie(_bench_IDS, _bench_CBS);
#  else
#    error "Define one of STRATEGY_PH, STRATEGY_SA, STRATEGY_TRIE when using COMPILE_BENCH"
#  endif

constexpr size_t bench_size_bytes = sizeof(_bench_table);
#endif // COMPILE_BENCH
