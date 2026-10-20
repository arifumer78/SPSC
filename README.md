# SPSC queue

A lock-free, bounded single-producer single-consumer queue in C++23, built on a fixed-size ring buffer and `std::atomic` with acquire/release ordering. No locks, no heap allocation, no system calls on the hot path.

Two cache-aware optimizations, each measured separately, made it about **3.3x faster** than the straightforward version: from ~81 million to ~265 million items per second.

## Usage

```cpp
SPSCQ<int, 1024> q;          // element type and number of slots, fixed at compile time

// producer thread only
while (!q.push(42)) std::this_thread::yield();   // false means full: retry

// consumer thread only
if (auto v = q.pop()) use(*v);                   // std::nullopt means empty
```

Exactly one thread may call `push` and exactly one other thread may call `pop`. Any other use is a data race.

## Build and run

```bash
g++ -std=c++23 -O2 -Wall -Wextra -Werror -pthread main.cpp -o spsc
./spsc
```

The test pushes 10 million integers through a 1024-slot queue from one thread, pops them in another, checks every value arrives in order, and prints the elapsed time and throughput.

Check for data races with ThreadSanitizer:

```bash
g++ -std=c++23 -O1 -g -fsanitize=thread -pthread main.cpp -o spsc_tsan
./spsc_tsan
```

## Design

### Ring buffer

A `std::array<T, N>` used as a circle, with two indices that only move forward and wrap with `% N`:

- `tail_`: the slot the producer writes next. Only the producer modifies it.
- `head_`: the slot the consumer reads next. Only the consumer modifies it.

| State | Condition |
| --- | --- |
| Empty | `head == tail` |
| Full | `(tail + 1) % N == head` |

One slot is always left unused, so a queue with `N` slots holds `N - 1` items. Without it, a full queue would also satisfy `head == tail` and be indistinguishable from an empty one.

### Why no locks are needed

Every variable has exactly one writer. The producer never writes `head_`, the consumer never writes `tail_`, and each only reads the other's index to check for full or empty. With no variable written by two threads, there is nothing for a lock to protect. What remains is visibility: each thread must see the other's writes in the right order.

### Memory ordering

| Index | Written by (`release`) | Read by (`acquire`) | Guarantees |
| --- | --- | --- | --- |
| `tail_` | producer, after writing the item | consumer, before reading the item | an item is fully written before the consumer can see it |
| `head_` | consumer, after reading the item | producer, before reusing the slot | an item is fully read before the producer overwrites it |

A release store synchronizes with an acquire load that reads the stored value: once the reader sees the new index, it also sees everything the writer did before publishing it. Each thread reads its own index with `memory_order_relaxed`, since a thread always sees its own latest writes.

Without the release/acquire pairing, the compiler or CPU could make the new `tail_` visible before the item in its slot, and the consumer would read garbage; or make the new `head_` visible before the consumer finished reading, and the producer would overwrite an item still in use.

## Optimizations

### 1. Avoiding false sharing

CPUs move memory between cores in 64-byte cache lines, not individual variables. `head_` and `tail_` originally sat in the same line, so every producer write to `tail_` invalidated the consumer's copy of `head_` and the reverse, and the line bounced between cores although neither thread touched the other's variable.

Each member now starts on its own cache line with `alignas(64)`. (C++17's `std::hardware_destructive_interference_size` names this value, but GCC warns about it under `-Werror` because it can differ between compilers; 64 is correct for x86.)

### 2. Cached indices

Every `push` still read the consumer's `head_` and every `pop` the producer's `tail_`: real sharing, one cross-core transfer per item.

Each side now keeps a private copy of the other's index (`head_cache_` for the producer, `tail_cache_` for the consumer) and reloads the real atomic only when its copy says the queue is full or empty. This is safe because the indices only move forward, so a stale value is pessimistic, never wrong: an old `head_` can only under-report free space, and an old `tail_` can only under-report available items. The caches are plain `std::size_t`, since each is used by a single thread, and each has its own cache line.

Memory ordering is preserved: when a cached value is used, the acquire load that filled it has already synchronized with the matching release, so everything published up to that index is visible.

In a busy queue, the expensive cross-core read now happens once per batch of items instead of once per item.

## Benchmark

10 million `int`s through a 1024-slot queue, producer and consumer on separate threads, `yield()` when full or empty. Built with `g++ -std=c++23 -O2`. Environment: WSL2 on a Windows laptop. Each version was run 5 or 6 times; the table shows the median, because thread placement and CPU frequency make single runs noisy.

| Version | Range | Median | Items/sec | Per item |
| --- | --- | --- | --- | --- |
| Baseline | 91–170 ms | ~123 ms | ~81M | ~12.4 ns |
| + `alignas(64)` | 102–115 ms | ~106 ms | ~94M | ~10.6 ns |
| + cached indices | 35–40 ms | ~38 ms | ~265M | ~3.8 ns |

Observations:

- Removing false sharing gained about 14% and also made runs far more consistent: the range shrank from 91–170 ms to 102–115 ms, because performance no longer depended on which cores the threads landed on.
- Cached indices gave about 2.8x on top. The remaining cost was never the arithmetic or the array access; it was data moving between cores.

## Limitations

- Exactly one producer thread and one consumer thread.
- `T` must be default-constructible and copy-assignable; `push` copies the item and `pop` copies it out.
- Capacity is fixed at compile time and holds `N - 1` items.
- Waiting on full or empty is left to the caller; the test busy-waits with `yield()`, which keeps a core busy.

## Next steps

- Replace `% N` with `& (N - 1)`, requiring `N` to be a power of two (`static_assert`)
- `push(T&&)` and `emplace` to move items instead of copying, and move out in `pop`
- Benchmark against a `std::mutex` + `std::queue` implementation
- Count cache misses with `perf stat -e cache-misses` for each version