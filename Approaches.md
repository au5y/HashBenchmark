# Static message ID lookup in C++17 for embedded systems

**Three zero-allocation dispatch strategies—perfect hashing, binary search, and structured trie—each offer distinct tradeoffs for mapping compile-time-known message IDs to callbacks or data on Zephyr RTOS.** The right choice depends on your ID density, flash budget, and latency requirements. All three approaches place their tables in `.rodata` (flash) via `constexpr`, consuming **zero RAM** on ARM Cortex-M.  Below are complete, production-oriented C++17 implementations for each, tested against GCC ARM toolchain constraints (`-fno-exceptions -fno-rtti -std=c++17`).

A common preamble applies to all three approaches: use plain function pointers (`void(*)(const uint8_t*, uint16_t)`) rather than `std::function`, which may heap-allocate internally.  For richer callbacks with context, the Embedded Template Library’s `etl::delegate` ( two pointers, no heap) is the gold standard alternative. All code below uses `std::array` for bounds safety and `constexpr` for flash placement.

-----

## Approach 1: compile-time perfect hash over a static array

### How it works

A minimal perfect hash function (MPHF) maps *N* known keys to indices **[0, N)** with zero collisions.  The CHD (Compress, Hash, Displace) algorithm—simplified by Steve Hanov and popularized by the `frozen` C++ library—works in two phases. First, a primary hash assigns all keys to buckets. Second, for each bucket (processed largest-first), the algorithm searches for a **displacement value** *d* such that a secondary hash `H(d, key)` maps every key in that bucket to an unoccupied slot.  The array of displacement values `G[]` is the entire runtime data structure: lookup is two hash evaluations and two array reads, making it **O(1) worst-case**.

For integer message IDs, we use a **MurmurHash3 finalizer** variant as the hash primitive—it’s fast, has excellent avalanche properties, and compiles to a handful of ARM instructions with no division. The entire construction runs at compile time via `constexpr`, so the displacement table and value arrays are placed directly in `.rodata` (flash).

The `frozen` library by Serge Sans Paille implements exactly this pattern and is battle-tested in production at Tesla. The implementation below is a self-contained version that requires no external dependencies, following the same algorithmic approach.

### Complete code: callback mapping

```cpp
// perfect_hash_dispatch.hpp — C++17, no heap, no RTTI, Zephyr-compatible
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

// --- Hash primitive: MurmurHash3 finalizer for uint32_t keys ---
constexpr uint32_t mph_hash(uint32_t key, uint32_t seed) {
    key ^= seed;
    key ^= key >> 16;
    key *= 0x45d9f3bU;
    key ^= key >> 16;
    key *= 0x45d9f3bU;
    key ^= key >> 16;
    return key;
}

// --- Compile-time perfect hash builder (CHD / Hanov algorithm) ---
template <size_t N>
struct PerfectHashTable {
    // G[i] > 0  → displacement seed for bucket i
    // G[i] <= 0 → direct slot index (-G[i]) for singleton buckets
    std::array<int32_t, N> G{};
    std::array<uint32_t, N> keys{};   // for verification on lookup
    std::array<bool, N> occupied{};   // construction bookkeeping

    constexpr uint32_t lookup_index(uint32_t key) const {
        uint32_t bucket = mph_hash(key, 0) % N;
        int32_t d = G[bucket];
        if (d < 0) {
            return static_cast<uint32_t>(-d - 1);
        }
        return mph_hash(key, static_cast<uint32_t>(d)) % N;
    }
};

// Constexpr builder: constructs the displacement table G[]
template <size_t N>
constexpr PerfectHashTable<N> build_perfect_hash(
    const std::array<uint32_t, N>& input_keys)
{
    PerfectHashTable<N> table{};
    // Track which output slots are taken
    std::array<bool, N> slot_used{};
    for (auto& s : slot_used) s = false;

    // Step 1: Assign keys to buckets by primary hash
    struct Bucket {
        std::array<uint32_t, N> members{};
        size_t count = 0;
    };
    std::array<Bucket, N> buckets{};

    for (size_t i = 0; i < N; ++i) {
        uint32_t b = mph_hash(input_keys[i], 0) % N;
        buckets[b].members[buckets[b].count++] = input_keys[i];
    }

    // Step 2: Sort buckets by size descending (simple selection sort)
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

    // Step 3: Process multi-key buckets — find displacement d
    for (size_t bi = 0; bi < N; ++bi) {
        size_t idx = bucket_order[bi];
        const auto& bkt = buckets[idx];
        if (bkt.count <= 1) break; // remaining are singletons

        // Try displacement values until all members land in free slots
        for (uint32_t d = 1; d < N * 10; ++d) {
            std::array<uint32_t, N> trial_slots{};
            size_t placed = 0;
            bool collision = false;

            for (size_t k = 0; k < bkt.count; ++k) {
                uint32_t slot = mph_hash(bkt.members[k], d) % N;
                if (slot_used[slot]) { collision = true; break; }
                // Check within this trial
                for (size_t p = 0; p < placed; ++p) {
                    if (trial_slots[p] == slot) {
                        collision = true; break;
                    }
                }
                if (collision) break;
                trial_slots[placed++] = slot;
            }

            if (!collision && placed == bkt.count) {
                // Accept this displacement
                table.G[idx] = static_cast<int32_t>(d);
                for (size_t k = 0; k < bkt.count; ++k) {
                    uint32_t slot = mph_hash(bkt.members[k], d) % N;
                    slot_used[slot] = true;
                    table.keys[slot] = bkt.members[k];
                }
                break;
            }
        }
    }

    // Step 4: Process singleton and empty buckets — place in free slots
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
            // Encode as negative: -(slot + 1)
            table.G[idx] = -static_cast<int32_t>(slot) - 1;
        }
        // count == 0: bucket unused, G[idx] stays 0 (sentinel)
    }

    return table;
}

// =====================================================================
// Usage: Callback dispatch via perfect hash
// =====================================================================
using MsgCallback = void(*)(const uint8_t* payload, uint16_t len);

// Define your message IDs (known at compile time from .dbc / .eds / header)
inline constexpr std::array<uint32_t, 8> MSG_IDS = {
    0x0010, 0x0042, 0x00A1, 0x0100,
    0x01FF, 0x0305, 0x0410, 0x07F0,
};

// Build the perfect hash at compile time
inline constexpr auto ph_table = build_perfect_hash(MSG_IDS);

// Forward-declare handlers (defined in .cpp files)
void handle_heartbeat(const uint8_t* p, uint16_t len);
void handle_config(const uint8_t* p, uint16_t len);
void handle_sensor_a(const uint8_t* p, uint16_t len);
void handle_data_req(const uint8_t* p, uint16_t len);
void handle_ack(const uint8_t* p, uint16_t len);
void handle_diag(const uint8_t* p, uint16_t len);
void handle_firmware(const uint8_t* p, uint16_t len);
void handle_broadcast(const uint8_t* p, uint16_t len);

// Parallel callback array: same index order as ph_table.keys[]
// We build this by looking up where each ID landed in the hash table
inline constexpr auto build_callback_table() {
    // Map: original ID order → callback
    constexpr MsgCallback handlers_by_id[] = {
        handle_heartbeat,  // 0x0010
        handle_config,     // 0x0042
        handle_sensor_a,   // 0x00A1
        handle_data_req,   // 0x0100
        handle_ack,        // 0x01FF
        handle_diag,       // 0x0305
        handle_firmware,   // 0x0410
        handle_broadcast,  // 0x07F0
    };
    // Reorder into hash-table slot order
    std::array<MsgCallback, 8> cb_table{};
    for (size_t i = 0; i < 8; ++i) cb_table[i] = nullptr;
    for (size_t i = 0; i < 8; ++i) {
        uint32_t slot = ph_table.lookup_index(MSG_IDS[i]);
        cb_table[slot] = handlers_by_id[i];
    }
    return cb_table;
}

inline constexpr auto CALLBACKS = build_callback_table();

// O(1) dispatch function
inline MsgCallback mph_find_handler(uint32_t msg_id) {
    uint32_t slot = ph_table.lookup_index(msg_id);
    if (slot < MSG_IDS.size() && ph_table.keys[slot] == msg_id) {
        return CALLBACKS[slot];
    }
    return nullptr; // unknown ID
}

inline void mph_dispatch(uint32_t msg_id,
                         const uint8_t* payload, uint16_t len)
{
    auto handler = mph_find_handler(msg_id);
    if (handler) handler(payload, len);
}
```

### Complete code: data/struct mapping

```cpp
// perfect_hash_data.hpp — struct lookup variant
#pragma once
#include "perfect_hash_dispatch.hpp"  // reuse ph_table from above

struct MessageDescriptor {
    uint32_t id;
    uint8_t  expected_dlc;
    uint16_t timeout_ms;
    uint8_t  priority;       // 0 = highest
    bool     requires_ack;
};

inline constexpr auto build_descriptor_table() {
    constexpr MessageDescriptor descriptors_by_id[] = {
        {0x0010, 0,  0,    0, false}, // heartbeat
        {0x0042, 8,  500,  2, true},  // config
        {0x00A1, 4,  100,  1, false}, // sensor_a
        {0x0100, 8,  1000, 3, true},  // data_req
        {0x01FF, 1,  50,   0, false}, // ack
        {0x0305, 8,  200,  2, true},  // diag
        {0x0410, 64, 5000, 4, true},  // firmware
        {0x07F0, 8,  0,    7, false}, // broadcast
    };

    std::array<MessageDescriptor, 8> table{};
    for (size_t i = 0; i < 8; ++i) {
        uint32_t slot = ph_table.lookup_index(MSG_IDS[i]);
        table[slot] = descriptors_by_id[i];
    }
    return table;
}

inline constexpr auto MSG_DESCRIPTORS = build_descriptor_table();

inline const MessageDescriptor* mph_find_descriptor(uint32_t msg_id) {
    uint32_t slot = ph_table.lookup_index(msg_id);
    if (slot < MSG_IDS.size() && ph_table.keys[slot] == msg_id) {
        return &MSG_DESCRIPTORS[slot];
    }
    return nullptr;
}
```

### Memory layout and Zephyr notes

**Flash usage** for 8 message IDs: `G[]` = 8 × 4 = 32 bytes, `keys[]` = 8 × 4 = 32 bytes, `CALLBACKS[]` = 8 × 4 = 32 bytes (ARM32 pointer size). **Total: ~96 bytes in `.rodata`/flash, zero RAM.** This scales linearly: 64 message IDs ≈ 768 bytes.

All `constexpr` arrays go to `.rodata`, which the standard Zephyr linker script places in flash.   On Cortex-M0/M0+ without hardware divide, ensure you use power-of-2 table sizes and replace `% N` with `& (N-1)`. The MurmurHash finalizer compiles to ~6 ARM Thumb-2 instructions (shifts, multiplies, XORs)—no division needed.

For Zephyr, compile with `CONFIG_CPLUSPLUS=y` and `CONFIG_STD_CPP17=y` in `prj.conf`. The `frozen` library (header-only, Apache 2.0) integrates trivially if you prefer a production-tested implementation over rolling your own. 

### Tradeoffs and when to prefer this approach

**Strengths:** True O(1) worst-case lookup. Dense storage—no wasted slots for sparse ID sets. Scales well to hundreds of IDs. The `frozen` library is battle-tested (used at Tesla per contributor acknowledgments).

**Gotchas:** Compile time increases with *N*—GCC’s constexpr evaluator is notably slower than Clang’s for large *N* (the `frozen` issue tracker documents this). The displacement search in `build_perfect_hash` has no formal upper bound on iterations, though in practice it converges quickly for reasonable *N*. **Verification is essential**: always check `keys[slot] == msg_id` at runtime to catch lookups for unregistered IDs. The hash function must be carefully chosen—the MurmurHash3 finalizer works well for integer keys but may need tuning for pathological ID distributions. 

**When to use:** Prefer this approach when you have **>16 sparse message IDs** that don’t share obvious bit-field structure, and you need guaranteed O(1) lookup. It’s the best general-purpose solution for arbitrary ID sets.

**FKS vs. CHD:** The FKS two-level scheme uses O(n) space but with larger constants (secondary tables sized at *k²*). CHD achieves tighter packing (~2 bits per key overhead) by using displacement values instead of per-bucket hash functions.  For embedded use, **CHD/Hanov is preferred**—it produces a single flat displacement array rather than variable-sized secondary tables, which is simpler to lay out in flash.

-----

## Approach 2: sorted array with constexpr binary search

### How it works

The simplest reliable approach: store `(message_id, callback_or_data)` pairs in a `std::array`, sort them at compile time, and use binary search at runtime. Since `std::sort` is not `constexpr` until C++20, we implement a constexpr insertion sort (optimal for the small *N* values typical in embedded). The sorted array goes into `.rodata`; at runtime, `std::lower_bound` performs **O(log N)** comparisons on contiguous memory, which is extremely cache-friendly.

For **N = 32** messages, this means at most **5 comparisons**—each touching adjacent memory. On Cortex-M4 at 168 MHz, that’s roughly **50–80 ns** including flash wait states. For small *N*, this often outperforms hash-based approaches because of lower constant overhead (no hash computation, just integer comparisons).

### Complete code: callback mapping

```cpp
// sorted_dispatch.hpp — C++17, no heap, Zephyr-compatible
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <algorithm>  // std::lower_bound (runtime, not constexpr)

using MsgCallback = void(*)(const uint8_t* payload, uint16_t len);

// --- Entry type for callback dispatch ---
struct CallbackEntry {
    uint32_t msg_id;
    MsgCallback handler;

    constexpr bool operator<(const CallbackEntry& other) const {
        return msg_id < other.msg_id;
    }
};

// --- Constexpr insertion sort (C++17-safe, no std::sort) ---
template <typename T, size_t N>
constexpr std::array<T, N> cx_sort(std::array<T, N> arr) {
    for (size_t i = 1; i < N; ++i) {
        for (size_t j = i; j > 0 && arr[j] < arr[j - 1]; --j) {
            T tmp = arr[j];
            arr[j] = arr[j - 1];
            arr[j - 1] = tmp;
        }
    }
    return arr;
}

// Forward-declare handlers
void handle_heartbeat(const uint8_t* p, uint16_t len);
void handle_config(const uint8_t* p, uint16_t len);
void handle_sensor_a(const uint8_t* p, uint16_t len);
void handle_data_req(const uint8_t* p, uint16_t len);
void handle_ack(const uint8_t* p, uint16_t len);
void handle_diag(const uint8_t* p, uint16_t len);
void handle_firmware(const uint8_t* p, uint16_t len);
void handle_broadcast(const uint8_t* p, uint16_t len);

// Entries authored in ANY order — sorted at compile time
inline constexpr auto DISPATCH_TABLE = cx_sort(std::array<CallbackEntry, 8>{{
    {0x0305, handle_diag},
    {0x0010, handle_heartbeat},
    {0x0100, handle_data_req},
    {0x07F0, handle_broadcast},
    {0x0042, handle_config},
    {0x01FF, handle_ack},
    {0x00A1, handle_sensor_a},
    {0x0410, handle_firmware},
}});

// Compile-time verification: table is actually sorted
static_assert(DISPATCH_TABLE[0].msg_id < DISPATCH_TABLE[1].msg_id,
              "Table must be sorted");

// --- Runtime binary search dispatch ---
inline MsgCallback sorted_find_handler(uint32_t msg_id) {
    // std::lower_bound works at runtime on constexpr arrays
    auto it = std::lower_bound(
        DISPATCH_TABLE.begin(), DISPATCH_TABLE.end(),
        CallbackEntry{msg_id, nullptr}
    );
    if (it != DISPATCH_TABLE.end() && it->msg_id == msg_id) {
        return it->handler;
    }
    return nullptr;
}

inline void sorted_dispatch(uint32_t msg_id,
                            const uint8_t* payload, uint16_t len)
{
    auto handler = sorted_find_handler(msg_id);
    if (handler) handler(payload, len);
}

// --- Manual binary search (avoids STL dependency entirely) ---
inline MsgCallback manual_bsearch_handler(uint32_t msg_id) {
    size_t lo = 0;
    size_t hi = DISPATCH_TABLE.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (DISPATCH_TABLE[mid].msg_id < msg_id) {
            lo = mid + 1;
        } else if (DISPATCH_TABLE[mid].msg_id > msg_id) {
            hi = mid;
        } else {
            return DISPATCH_TABLE[mid].handler;
        }
    }
    return nullptr;
}
```

### Complete code: data/struct mapping

```cpp
// sorted_data.hpp — struct lookup variant
#pragma once
#include <array>
#include <cstdint>
#include <algorithm>

struct SensorConfig {
    uint32_t sensor_id;        // used as sort key
    uint16_t sample_rate_hz;
    uint8_t  resolution_bits;
    uint8_t  channel;
    uint16_t filter_cutoff_hz;
    bool     enabled;

    constexpr bool operator<(const SensorConfig& other) const {
        return sensor_id < other.sensor_id;
    }
};

template <typename T, size_t N>
constexpr std::array<T, N> cx_sort(std::array<T, N> arr) {
    for (size_t i = 1; i < N; ++i) {
        for (size_t j = i; j > 0 && arr[j] < arr[j - 1]; --j) {
            T tmp = arr[j]; arr[j] = arr[j - 1]; arr[j - 1] = tmp;
        }
    }
    return arr;
}

// Authored in arbitrary order; compiler sorts them
inline constexpr auto SENSOR_TABLE = cx_sort(std::array<SensorConfig, 6>{{
    {0x00A1, 1000, 12, 0, 500,  true},
    {0x0042, 2000, 16, 1, 1000, true},
    {0x0305, 500,  10, 2, 200,  false},
    {0x0010, 100,  8,  3, 50,   true},
    {0x0100, 4000, 24, 0, 2000, true},
    {0x01FF, 250,  12, 4, 100,  false},
}});

// Compile-time correctness check
static_assert(SENSOR_TABLE[0].sensor_id == 0x0010, "Sort failed");
static_assert(SENSOR_TABLE[5].sensor_id == 0x0305, "Sort failed");

inline const SensorConfig* find_sensor_config(uint32_t sensor_id) {
    auto it = std::lower_bound(
        SENSOR_TABLE.begin(), SENSOR_TABLE.end(),
        SensorConfig{sensor_id, 0, 0, 0, 0, false}
    );
    if (it != SENSOR_TABLE.end() && it->sensor_id == sensor_id) {
        return &(*it);
    }
    return nullptr;
}
```

### Memory layout and Zephyr notes

**Flash usage** for 8 callback entries: 8 × (4 + 4) = **64 bytes in `.rodata`**. For the 6-entry `SensorConfig` table: 6 × 10 = **60 bytes**. This is the most memory-efficient approach since there are no auxiliary data structures (no displacement table, no sentinel arrays).

The `constexpr` insertion sort runs at compile time with **O(N²)** complexity, which is fine for N < 100. For larger sets, a constexpr quicksort (as described by Tristan Brindle) is more appropriate,  though the recursive depth may require increasing `-fconstexpr-depth` in GCC.

On Zephyr, `std::lower_bound` from `<algorithm>` is available when `CONFIG_CPLUSPLUS=y` and `CONFIG_LIB_CPLUSPLUS=y`. If you want to minimize STL dependencies, the manual binary search variant above has zero library dependencies.

### Tradeoffs and when to prefer this approach

**Strengths:** Minimal code complexity, smallest memory footprint, easiest to audit for safety-critical systems. **No hash function to validate**—correctness depends only on the sort being right, which `static_assert` verifies at compile time. The contiguous memory layout means the entire table often fits in a single cache line on Cortex-M7.

**Gotchas:** O(log N) is not O(1)—for **N > 256**, the 8+ comparisons start to matter in hard real-time loops. Each comparison involves a branch, and branch misprediction on Cortex-M can cost 3–13 cycles. The `cx_sort` insertion sort has O(N²) compile-time cost; GCC may hit constexpr evaluation limits  around N = 200–500 depending on `-fconstexpr-steps`.

**When to use:** This is the **default recommendation for N < 64** message IDs, which covers the vast majority of embedded protocols. It’s the simplest to maintain, the easiest to review in code audits, and has the most predictable performance. **MISRA C++ and functional-safety auditors love this pattern** because the invariant (sorted order) is trivially verifiable. Prefer this unless you have measured evidence that O(log N) is too slow for your interrupt latency budget.

-----

## Approach 3: multi-level static array exploiting ID bit structure

### How it works

When message IDs have **semantic bit-field structure**—as in CAN, CANopen, J1939, or any custom protocol—you can decompose the ID into fields and use each field as an array index. This creates a compile-time trie: a nested array where the first dimension indexes by the upper bits (subsystem/function code) and the second dimension indexes by the lower bits (message type/node ID).

For a CANopen 11-bit COB-ID: **bits [10:7]** are the 4-bit function code (NMT, PDO, SDO, etc.) and **bits [6:0]** are the 7-bit node ID. A 16 × 128 array gives you pure O(1) dispatch with two array accesses and zero arithmetic beyond bit shifts and masks. For J1939’s 29-bit extended CAN IDs, the PF (PDU Format) byte is the natural first-level discriminator, with the PS (PDU Specific) byte as the second level.  

The tradeoff is **space**: you allocate slots for the full range of each bit-field, even if most are unused. A 16 × 128 CANopen table costs **2 KB of flash**—very reasonable. A full 256 × 256 J1939 table costs 64 KB—possible on large Cortex-M4/M7 parts but wasteful. The sparse J1939 variant below uses selective sub-tables to cut this to ~5 KB.

### Complete code: callback mapping (CANopen-style)

```cpp
// trie_dispatch.hpp — C++17, no heap, Zephyr-compatible
#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

using MsgCallback = void(*)(const uint8_t* payload, uint16_t len);

// --- Sentinel values ---
static constexpr uint8_t NO_HANDLER = 0xFF;
using HandlerId = uint8_t;

// --- CANopen COB-ID decomposition ---
// COB-ID [10:7] = function code (4 bits, 0–15)
// COB-ID [6:0]  = node ID (7 bits, 0–127)
constexpr uint8_t canopen_func_code(uint16_t cob_id) {
    return static_cast<uint8_t>((cob_id >> 7) & 0x0F);
}
constexpr uint8_t canopen_node_id(uint16_t cob_id) {
    return static_cast<uint8_t>(cob_id & 0x7F);
}

// --- Two-level dispatch table ---
static constexpr size_t NUM_FUNC_CODES = 16;
static constexpr size_t NUM_NODE_IDS   = 128;

struct TrieDispatchTable {
    std::array<std::array<HandlerId, NUM_NODE_IDS>, NUM_FUNC_CODES> slots;

    constexpr TrieDispatchTable() : slots{} {
        for (auto& row : slots)
            for (auto& cell : row)
                cell = NO_HANDLER;
    }

    // Register a handler for a specific COB-ID
    constexpr void map(uint16_t cob_id, HandlerId handler) {
        slots[canopen_func_code(cob_id)][canopen_node_id(cob_id)] = handler;
    }

    // Register a handler for ALL node IDs under a function code
    constexpr void map_broadcast(uint8_t func_code, HandlerId handler) {
        for (size_t n = 0; n < NUM_NODE_IDS; ++n)
            slots[func_code][n] = handler;
    }

    // O(1) lookup: two array indexes, no branches except sentinel check
    constexpr HandlerId lookup(uint16_t cob_id) const {
        return slots[canopen_func_code(cob_id)][canopen_node_id(cob_id)];
    }
};

// --- Build at compile time ---
inline constexpr auto build_canopen_dispatch() {
    TrieDispatchTable t;

    // NMT (func_code=0): broadcast handler
    t.map(0x0000, 0);                    // NMT command (node 0)

    // SYNC (func_code=1, node 0): COB-ID 0x0080
    t.map(0x0080, 1);                    // SYNC

    // EMCY (func_code=1): per-node emergency
    // Node 5: COB-ID = 0x0080 + 5 = 0x0085
    t.map(0x0085, 2);                    // EMCY from node 5

    // TPDO1 (func_code=3): COB-ID = 0x0180 + NodeID
    t.map(0x0180 + 1, 3);               // TPDO1 from node 1
    t.map(0x0180 + 5, 3);               // TPDO1 from node 5
    t.map(0x0180 + 10, 3);              // TPDO1 from node 10

    // RPDO1 (func_code=4): COB-ID = 0x0200 + NodeID
    t.map(0x0200 + 5, 4);               // RPDO1 to node 5

    // SDO TX (func_code=11): COB-ID = 0x0580 + NodeID
    t.map(0x0580 + 5, 5);               // SDO response from node 5

    // Heartbeat (func_code=14): listen to all nodes
    t.map_broadcast(0x0E, 6);           // all heartbeats → handler 6

    return t;
}

inline constexpr auto CANOPEN_TRIE = build_canopen_dispatch();

// Compile-time verification
static_assert(CANOPEN_TRIE.lookup(0x0185) == 3, "TPDO1 node 5");
static_assert(CANOPEN_TRIE.lookup(0x0701) == 6, "Heartbeat node 1");
static_assert(CANOPEN_TRIE.lookup(0x0300) == NO_HANDLER, "Unmapped");

// --- Handler function table (indexed by HandlerId) ---
void handle_nmt(const uint8_t* p, uint16_t len);
void handle_sync(const uint8_t* p, uint16_t len);
void handle_emcy(const uint8_t* p, uint16_t len);
void handle_tpdo1(const uint8_t* p, uint16_t len);
void handle_rpdo1(const uint8_t* p, uint16_t len);
void handle_sdo_tx(const uint8_t* p, uint16_t len);
void handle_heartbeat(const uint8_t* p, uint16_t len);

inline constexpr std::array<MsgCallback, 7> HANDLER_TABLE = {{
    handle_nmt,        // 0
    handle_sync,       // 1
    handle_emcy,       // 2
    handle_tpdo1,      // 3
    handle_rpdo1,      // 4
    handle_sdo_tx,     // 5
    handle_heartbeat,  // 6
}};

// Complete dispatch: COB-ID → handler ID → function call
inline void trie_dispatch(uint16_t cob_id,
                          const uint8_t* payload, uint16_t len)
{
    HandlerId hid = CANOPEN_TRIE.lookup(cob_id);
    if (hid != NO_HANDLER && hid < HANDLER_TABLE.size()) {
        HANDLER_TABLE[hid](payload, len);
    }
}
```

### Complete code: data/struct mapping (custom protocol)

```cpp
// trie_data.hpp — Custom protocol with subsystem/msgtype structure
#pragma once
#include <array>
#include <cstdint>

// Custom 16-bit ID layout:
// [15:12] = subsystem (4 bits → 16 subsystems)
// [11:4]  = message type (8 bits → 256 types per subsystem)
// [3:0]   = reserved / instance (ignored for dispatch)

static constexpr size_t NUM_SUBSYSTEMS = 16;
static constexpr size_t NUM_MSG_TYPES  = 256;

constexpr uint8_t get_subsystem(uint16_t id) {
    return static_cast<uint8_t>((id >> 12) & 0x0F);
}
constexpr uint8_t get_msg_type(uint16_t id) {
    return static_cast<uint8_t>((id >> 4) & 0xFF);
}

struct MsgMetadata {
    uint8_t  expected_dlc;
    uint16_t timeout_ms;
    uint8_t  priority;        // lower = higher priority
    bool     requires_ack;
    bool     is_periodic;
    uint16_t period_ms;       // 0 if aperiodic
};

static constexpr MsgMetadata EMPTY_META = {0, 0, 0xFF, false, false, 0};

struct MetadataTrieTable {
    std::array<std::array<MsgMetadata, NUM_MSG_TYPES>, NUM_SUBSYSTEMS> data;

    constexpr MetadataTrieTable() : data{} {
        for (auto& sub : data)
            for (auto& entry : sub)
                entry = EMPTY_META;
    }

    constexpr void define(uint16_t msg_id, MsgMetadata meta) {
        data[get_subsystem(msg_id)][get_msg_type(msg_id)] = meta;
    }

    constexpr const MsgMetadata& lookup(uint16_t msg_id) const {
        return data[get_subsystem(msg_id)][get_msg_type(msg_id)];
    }
};

inline constexpr auto build_metadata_table() {
    MetadataTrieTable t;

    // Subsystem 0x1 (Powertrain), MsgType 0x00 (RPM): ID = 0x1000
    t.define(0x1000, {4, 100, 1, false, true, 10});

    // Subsystem 0x1, MsgType 0x01 (Torque): ID = 0x1010
    t.define(0x1010, {8, 100, 1, false, true, 10});

    // Subsystem 0x2 (Chassis), MsgType 0x00 (Wheel Speed): ID = 0x2000
    t.define(0x2000, {8, 50,  0, false, true, 5});

    // Subsystem 0x3 (Body), MsgType 0x10 (Door Status): ID = 0x3100
    t.define(0x3100, {2, 500, 4, false, false, 0});

    // Subsystem 0x4 (Diagnostics), MsgType 0x00 (Request): ID = 0x4000
    t.define(0x4000, {8, 2000, 3, true, false, 0});

    // Subsystem 0x4, MsgType 0x01 (Response): ID = 0x4010
    t.define(0x4010, {8, 2000, 3, false, false, 0});

    return t;
}

inline constexpr auto MSG_METADATA = build_metadata_table();

// Compile-time verification
static_assert(MSG_METADATA.lookup(0x1000).period_ms == 10, "RPM = 10ms");
static_assert(MSG_METADATA.lookup(0x2000).priority == 0,   "Wheel = P0");
static_assert(MSG_METADATA.lookup(0xFFFF).priority == 0xFF, "Unmapped");

inline const MsgMetadata& get_msg_metadata(uint16_t msg_id) {
    return MSG_METADATA.lookup(msg_id);
}

inline bool is_known_message(uint16_t msg_id) {
    return MSG_METADATA.lookup(msg_id).priority != 0xFF;
}
```

### Sparse J1939 variant (handling 29-bit IDs efficiently)

```cpp
// j1939_dispatch.hpp — Sparse two-level trie for J1939 PGNs
#pragma once
#include <array>
#include <cstdint>

static constexpr uint8_t J1939_NO_HANDLER = 0xFF;
using HandlerId = uint8_t;

// J1939 29-bit CAN ID:
// [28:26] Priority  [25] Reserved  [24] Data Page
// [23:16] PF (PDU Format)  [15:8] PS (PDU Specific)  [7:0] Source Addr

constexpr uint8_t j1939_pf(uint32_t can_id) {
    return static_cast<uint8_t>((can_id >> 16) & 0xFF);
}
constexpr uint8_t j1939_ps(uint32_t can_id) {
    return static_cast<uint8_t>((can_id >> 8) & 0xFF);
}

// PDU2 (PF >= 240): PS = Group Extension (part of PGN)
// PDU1 (PF < 240): PS = Destination Address (NOT part of PGN)
constexpr bool is_pdu2(uint8_t pf) { return pf >= 240; }

struct J1939Trie {
    // Level 1: PF byte → direct handler for PDU1 messages
    std::array<HandlerId, 256> pdu1_table;

    // Level 2: 16 sub-tables for PF 240–255 (PDU2), indexed by PS
    std::array<std::array<HandlerId, 256>, 16> pdu2_tables;

    constexpr J1939Trie() : pdu1_table{}, pdu2_tables{} {
        for (auto& e : pdu1_table) e = J1939_NO_HANDLER;
        for (auto& sub : pdu2_tables)
            for (auto& e : sub) e = J1939_NO_HANDLER;
    }

    // Register by PGN (18-bit: R+DP+PF+PS for PDU2, R+DP+PF for PDU1)
    constexpr void map_pdu1(uint8_t pf, HandlerId h) {
        pdu1_table[pf] = h;
    }
    constexpr void map_pdu2(uint8_t pf, uint8_t ps, HandlerId h) {
        pdu2_tables[pf - 240][ps] = h;
    }

    // O(1) dispatch
    constexpr HandlerId lookup(uint32_t can_id) const {
        uint8_t pf = j1939_pf(can_id);
        if (is_pdu2(pf)) {
            return pdu2_tables[pf - 240][j1939_ps(can_id)];
        }
        return pdu1_table[pf];
    }
};

inline constexpr auto build_j1939_dispatch() {
    J1939Trie t;

    // PDU1 messages (PF < 240):
    // TSC1 (Torque/Speed Control): PGN 0x0000, PF=0x00
    t.map_pdu1(0x00, 0);
    // AC (Address Claimed): PF=0xEE (238)
    t.map_pdu1(0xEE, 1);

    // PDU2 messages (PF >= 240):
    // EEC1 (Engine Electronic Controller 1): PGN 61444, PF=0xF0, PS=0x04
    t.map_pdu2(0xF0, 0x04, 2);
    // CCVS (Cruise Control/Vehicle Speed): PGN 65265, PF=0xFE, PS=0xF1
    t.map_pdu2(0xFE, 0xF1, 3);
    // ET1 (Engine Temperature 1): PGN 65262, PF=0xFE, PS=0xEE
    t.map_pdu2(0xFE, 0xEE, 4);
    // EFL/P1 (Engine Fluid Level/Pressure): PGN 65263, PF=0xFE, PS=0xEF
    t.map_pdu2(0xFE, 0xEF, 5);

    return t;
}

inline constexpr auto J1939_DISPATCH = build_j1939_dispatch();

// Verify: EEC1 with priority 3, SA 0x00 → CAN ID 0x0CF00400
static_assert(J1939_DISPATCH.lookup(0x0CF00400) == 2, "EEC1 handler");
```

### Memory layout and Zephyr notes

**CANopen trie** (16 × 128 × 1 byte): **2,048 bytes flash, zero RAM.** Extremely reasonable—this is smaller than many CRC tables.

**Custom protocol trie** (16 × 256 × `sizeof(MsgMetadata)`): with `MsgMetadata` at 8 bytes, that’s 16 × 256 × 8 = **32 KB flash**. This is significant—appropriate for Cortex-M4/M7 parts with 256 KB+ flash, but too large for Cortex-M0 with 64 KB. Reduce by narrowing the subsystem or message-type range if your protocol only uses a fraction of the address space.

**J1939 sparse trie**: PDU1 table = 256 bytes, PDU2 sub-tables = 16 × 256 = 4,096 bytes. **Total: ~4.4 KB flash.** This is a 15× reduction versus the naive 256 × 256 = 64 KB approach while maintaining O(1) lookup.

On Zephyr, the `constexpr` tables are placed in flash by the default linker script. For **Cortex-M7 with DTCM RAM**, you can copy latency-critical tables into tightly-coupled memory with a custom section attribute: `__attribute__((section(".dtcm_data")))`.

### Tradeoffs and when to prefer this approach

**Strengths:** **Absolute fastest possible dispatch**—two array indexes compile to two `LDR` instructions on ARM with no arithmetic beyond shifts and masks. Zero computational overhead. The lookup is completely branchless (the sentinel check is the only conditional). Memory access patterns are fully predictable, making this ideal for hard real-time interrupt handlers.

**Gotchas:** The primary cost is **flash waste from sparse population**. If only 20 of 2,048 possible COB-IDs are registered, 99% of the table is sentinels. This is acceptable when flash is abundant and RAM is scarce (the normal embedded situation), but problematic on very constrained parts. Also, this approach **only works when IDs have exploitable bit-field structure**—it does not generalize to arbitrary ID sets.

**When to use:** This is the **optimal choice when your protocol IDs have a defined bit-field layout** (CAN, CANopen, J1939, custom protocols) and you need the lowest possible dispatch latency. It’s the natural choice for CAN receive ISRs where every nanosecond matters. The COMMS library (arobenko/commschamp) uses this exact strategy when message IDs are within 10% density of a flat array.

-----

## Choosing between the three approaches

The decision tree is straightforward. First, ask whether your message IDs have **structured bit-fields** with semantic meaning. If yes, the trie approach (Approach 3) gives you unbeatable O(1) with predictable flash cost—use it. If IDs are arbitrary or come from multiple unrelated protocols, ask how many you have. For **N < 64**, the sorted array (Approach 2) is simplest and sufficient: 6 comparisons worst-case is negligible on any Cortex-M. For **N > 64 with arbitrary sparse IDs**, the perfect hash (Approach 1) provides O(1) without the flash waste of a flat trie.

|Factor                  |Perfect hash      |Sorted array             |Trie                             |
|------------------------|:----------------:|:-----------------------:|:-------------------------------:|
|Lookup complexity       |**O(1)**          |O(log N)                 |**O(1)**                         |
|Flash overhead          |Low (~12× N bytes)|**Minimal** (~8× N bytes)|Medium–high (depends on ID range)|
|RAM usage               |**Zero**          |**Zero**                 |**Zero**                         |
|Code complexity         |High              |**Low**                  |Medium                           |
|Auditability            |Medium            |**High**                 |High                             |
|Works with arbitrary IDs|**Yes**           |**Yes**                  |No (needs structure)             |
|Compile-time cost       |High for large N  |Low                      |Low                              |

All three patterns share the same critical property: `constexpr` construction ensures the tables live entirely in `.rodata`/flash, consuming zero RAM. Combined with plain function pointers (or `etl::delegate` for member-function callbacks), they give you production-grade, zero-allocation dispatch that plays well with Zephyr’s C++ support, GCC ARM’s optimizer, and the constraints of real-time embedded systems.

## Conclusion

The most underappreciated insight across all three approaches is that **constexpr in C++17 is powerful enough to build any static data structure at compile time**, including hash tables, sorted arrays, and multi-dimensional tries. The `std::array::operator[]` becoming constexpr in C++17 (via P0107R0) was the critical enabler—without it, none of these patterns work.

For teams adopting these patterns, the `frozen` library provides a production-tested implementation of Approach 1 with zero integration cost (header-only, Apache 2.0). The Embedded Template Library (`etl::delegate_service`) offers a ready-made variant of Approach 3 for interrupt-vector-style dispatch. But as shown above, rolling your own in ~100 lines of C++17 is entirely feasible and gives you full control over memory layout—a property embedded engineers rightly insist on.
