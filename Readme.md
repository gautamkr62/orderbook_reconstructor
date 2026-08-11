# Order Book Reconstructor

A real-time, self-validating order book feed handler for Binance spot markets.

## Overview

This project builds and maintains a correct, low-latency L2 order book for crypto trading pairs (e.g., `DOGEUSDT`, `BTCUSDT`) using Binance's REST snapshot + WebSocket depth stream.

It handles the notorious race conditions, sequence gaps, and API limitations that plague naive implementations — and automatically recovers when things go wrong.

## Why This Project Matters

Binance's WebSocket stream sends deltas (price-level changes), not full books.

The REST snapshot is a point-in-time photograph, but the WebSocket stream starts before you receive it.

Network drops and out-of-order messages are inevitable.

This system merges the two sources correctly, forever — and proves it with a background validator.

## Features

| Feature | Description |
|---|---|
| **Real-time L2 order book** | Tick-indexed vector + bitmap for O(1) updates and cache-efficient storage. |
| **Self-healing bootstrap** | Connects WebSocket first, buffers deltas, then applies REST snapshot and replays buffered history. |
| **Sequence-number gap detection** | Uses Binance's `U` and `u` fields to detect lost messages and trigger automatic resync. |
| **Background validator** | Every 30–60 seconds, fetches a fresh REST snapshot, builds a shadow book, replays history, and compares it to the live book — proving correctness. |
| **Three-thread architecture** | IO thread (network), Engine thread (reconciliation), Validator thread (audit) — completely decoupled. |
| **Symbol-agnostic** | Works for any Binance spot symbol (`DOGEUSDT`, `BTCUSDT`, `ETHUSDT`, etc.). |
| **Performance metrics** | Built-in latency tracking (p50/p99), validation success/mismatch counters, and active level counts. |
| **Clean shutdown** | Handles `SIGINT` gracefully, joins all threads, and closes WebSocket properly. |

## Architecture Overview

```text
[Binance REST API]          [Binance WebSocket]
        │                           │
        ▼                           ▼
  RestClient                WebSocketClient
        │                           │
        │ (snapshot)                 │ (deltas)
        ▼                           ▼
        └───────────► ThreadSafeQueue ◄────────┘
                           │
                           ▼
                    [Engine Thread]
                           │
                ┌──────────┼──────────┐
                ▼          ▼          ▼
          [OrderBook] [DiffHistoryBuffer] [Reconciler]
                           │
                           ▼
                    [Validator Thread]
                           │
                           ▼
                    [Shadow Book] ─── compares ───► [Live Book]
```

## Threads

| Thread | Role |
|---|---|
| **IO Thread** | Runs `boost::asio::io_context`. Reads WebSocket messages, pushes raw JSON into a thread-safe queue. Never touches the order book. |
| **Engine Thread** | Owns the `OrderBook`. Consumes messages from the queue, parses them, updates the book, and stores parsed deltas in the `DiffHistoryBuffer`. Handles bootstrap and resync. |
| **Validator Thread** | Periodically freezes the live book, fetches a REST snapshot, builds a shadow book, replays history, and compares the two. Triggers resync if mismatches persist. |

## Key Components

| Component | Responsibility |
|---|---|
| **OrderBook** | Tick-indexed vector + bitmap for bids and asks. O(1) updates, cache-friendly. |
| **SideBook\<Side>** | Manages one side of the book (bids or asks). Handles window recentering, bitmap, and best-tick tracking. |
| **Reconciler** | Orchestrates bootstrap, steady-state delta processing, gap detection, and resync. |
| **Validator** | Background auditor — freeze → fetch → replay → compare. |
| **DiffHistoryBuffer** | Ring buffer of parsed `Delta` structs for replay. |
| **WebSocketClient** | State-machine based WebSocket client with reconnect and graceful shutdown. |
| **RestClient** | Asynchronous HTTPS client for REST snapshots (supports depth limit). |
| **MetricsCollector** | Tracks p50/p99 latency, validation counters, and active level counts. |

## Bootstrap and Reconciliation

The critical part of the system is correctly combining the REST snapshot with the WebSocket stream.

The WebSocket connection is established first. Incoming depth updates are buffered while the REST snapshot is fetched.

```text
                    WebSocket
                        │
                        ▼
                 Buffer deltas
                        │
                        │
                        │       REST
                        │      snapshot
                        │         │
                        │         ▼
                        │   Apply snapshot
                        │         │
                        └────────► │
                                  ▼
                         Replay buffered
                            deltas
                                  │
                                  ▼
                         Live Order Book
```

This avoids the race condition where WebSocket updates occur while the REST snapshot is being fetched.

The reconciler uses Binance's sequence fields:

- `U` — first update ID in the event
- `u` — final update ID in the event

The buffered events are replayed only when they correctly bridge the snapshot sequence.

If the sequence cannot be reconciled, the system discards the inconsistent state and performs a fresh bootstrap.

## Sequence Gap Detection

During steady-state operation, every incoming delta is checked against the last processed update ID.

Conceptually:

```text
Expected next update:
    last_u + 1

Incoming update:
    [U, u]
```

If an unexpected sequence gap is detected:

```text
Live Book
    │
    ▼
Sequence Gap Detected
    │
    ▼
Invalidate Current State
    │
    ▼
Fresh REST Snapshot
    │
    ▼
Replay Buffered WebSocket Deltas
    │
    ▼
Resume Live Processing
```

This prevents the system from silently continuing with an incomplete order book.

## Design Decisions

### Tick-Indexed Vector + Bitmap

Instead of a `std::map` or hash map, the order book uses a contiguous vector indexed by price ticks.

Conceptually:

```text
price
  │
  ▼
tick index
  │
  ▼
┌─────────────────────────────────────┐
│ PriceLevel │ PriceLevel │ PriceLevel │ ...
└─────────────────────────────────────┘
       │
       ▼
   Activity bitmap
```

Each tick has a `PriceLevel`, while a bitmap tracks whether that tick currently contains an active level.

This gives:

- O(1) random access for updates.
- Excellent cache locality.
- Reduced pointer chasing compared with tree-based containers.
- Efficient best-price discovery.
- Better CPU cache utilization for high-frequency updates.

Best bid/ask discovery uses CPU intrinsics such as:

```cpp
__builtin_ctzll()
__builtin_clzll()
```

to locate active ticks efficiently.

### Why not `std::map`?

A traditional tree-based structure introduces:

- O(log N) updates.
- Pointer chasing.
- Poorer cache locality.
- Higher allocation overhead.

For a high-frequency stream where price levels are updated continuously, the contiguous representation provides a more efficient update path.

## Three-Thread Architecture

The system intentionally separates network IO, book mutation, and validation.

### IO Thread

The IO thread runs:

```cpp
boost::asio::io_context
```

Its responsibilities are limited to:

1. Reading WebSocket frames.
2. Handling network events.
3. Reconnecting when necessary.
4. Pushing received messages into the thread-safe queue.

It never directly modifies the order book.

This keeps the socket event loop lightweight and avoids blocking network processing with CPU-heavy operations.

### Engine Thread

The Engine thread owns the `OrderBook`.

It:

1. Consumes messages from the queue.
2. Parses JSON using `simdjson`.
3. Converts messages into compact `Delta` structures.
4. Applies deltas to the order book.
5. Stores parsed deltas in the history buffer.
6. Performs sequence validation.
7. Handles bootstrap and resynchronization.

The single-owner model significantly simplifies synchronization around the order book.

### Validator Thread

The Validator periodically audits the live book.

Its workflow is:

```text
Freeze Live State
       │
       ▼
Fetch REST Snapshot
       │
       ▼
Build Shadow Book
       │
       ▼
Replay Historical Deltas
       │
       ▼
Compare Shadow vs Live
       │
       ├── Match ──► Validation Success
       │
       └── Mismatch ──► Investigate / Resync
```

The validator does not run on the critical update path.

## DiffHistoryBuffer

The validator needs historical WebSocket deltas to reconstruct the book from a known REST snapshot.

Instead of storing raw JSON messages, the system stores parsed `Delta` structures:

```text
Delta
├── U
├── u
├── bids[]
└── asks[]
```

These are stored in a bounded ring buffer.

Advantages:

- Lower memory usage.
- No repeated JSON parsing.
- Deterministic replay.
- Constant-memory history storage.
- Efficient sequential access.

Example:

```text
┌─────┬─────┬─────┬─────┬─────┬─────┐
│ D1  │ D2  │ D3  │ D4  │ D5  │ D6  │
└─────┴─────┴─────┴─────┴─────┴─────┘
  ▲
  │
 oldest
```

When the buffer reaches capacity, the oldest deltas are overwritten.

If the validator needs history that has already been evicted, that validation round is skipped rather than producing an invalid conclusion.

## Background Validation

A key property of this project is that it does not simply assume the reconstructed order book is correct.

Instead, it periodically independently reconstructs the book.

### Validation Workflow

```text
             Live Order Book
                    │
                    │ freeze point
                    ▼
              Freeze State
                    │
                    ▼
             REST Snapshot
                    │
                    ▼
             Shadow OrderBook
                    │
                    ▼
             Replay Deltas
                    │
                    ▼
          Shadow Book @ Freeze
                    │
                    ▼
             Compare Prices
                    │
             ┌──────┴──────┐
             ▼             ▼
           Match        Mismatch
             │             │
             ▼             ▼
          Success       Investigate
```

The validator compares the shadow book against the live book by **price level**, rather than by vector index.

This is important because the REST API has a finite depth limit.

## Price-Based Comparison

The validator compares:

```text
price → quantity
```

rather than:

```text
vector index → quantity
```

This avoids false mismatches when the live book contains more levels than the REST snapshot.

For example:

```text
REST snapshot:
5000 bid levels

Live book:
7000 bid levels
```

The validator can compare the overlapping price range without incorrectly assuming that both representations must have identical depth.

## Performance Metrics

The system tracks update-path latency and reports percentile metrics.

Example:

```text
========== Metrics Snapshot ==========
Deltas applied      : 16228
Validation success  : 520
Validation mismatches: 22
Validation skipped   : 0
Resyncs triggered    : 0
Active bids/asks    : 2458 / 5199
Latency (p50/p99)   : 15us / 68us
=======================================
```

| Field | Meaning |
|---|---|
| **Deltas applied** | Total WebSocket deltas processed since startup. |
| **Validation success** | Number of validator cycles that passed. |
| **Validation mismatches** | Number of validator cycles that detected a divergence (retry may resolve). |
| **Validation skipped** | Validator rounds skipped due to large gaps or missing history. |
| **Resyncs triggered** | Number of times the system rebuilt the book from scratch. |
| **Active bids/asks** | Current number of active bid and ask price levels in the live book. |
| **Latency (p50/p99)** | Delta-apply latency at the 50th and 99th percentile. |

## Limitations

### REST Depth Cap

Binance returns a limited number of price levels through the REST depth endpoint.

Therefore, deeper levels cannot be independently validated using the REST-based shadow-book approach.

The validator can only establish correctness within the available snapshot depth.

### L2 Only

This implementation reconstructs a **Market-by-Price (L2)** order book.

It tracks:

```text
Price → Aggregate Quantity
```

rather than individual orders.

It does not maintain:

```text
Order ID → Order
```

Therefore, it cannot reconstruct an L3 / Market-by-Order book.

### Single Symbol Per Instance

The current implementation handles one trading symbol per process instance.

For example:

```bash
./orderbook_demo BTCUSDT
```

Multi-symbol support would require multiple `Reconciler` instances or a symbol-aware centralized architecture.

### History Buffer Capacity

The validator relies on the `DiffHistoryBuffer`.

If the time between the REST snapshot and the frozen live state exceeds the history buffer capacity, the required deltas may have already been evicted.

In that situation, the validator skips the round rather than incorrectly declaring the order book invalid.

## Future Improvements

### Support for Multiple Symbols Concurrently

Run multiple order-book reconstructors concurrently:

```text
                ┌── BTCUSDT Reconciler
                │
Market Stream ──┼── ETHUSDT Reconciler
                │
                └── DOGEUSDT Reconciler
```

### Cross-Exchange Arbitrage Detection

Compare reconstructed order books across exchanges:

```text
Binance
   │
   ├── BTCUSDT
   │
   ▼
Normalized Order Book
   │
   ├──────────────┐
   │              │
   ▼              ▼
Bybit          Binance
   │              │
   └──────┬───────┘
          ▼
   Arbitrage Engine
```

Potential future integrations include Binance and Bybit.

### Persistent Storage of Metrics

Persist latency and validation metrics to CSV or a time-series database for long-term analysis.

This would allow analysis of:

- p50/p99 latency over time.
- Resynchronization frequency.
- Validation mismatch frequency.
- Message throughput.
- Book depth.
- Network instability.

### L3 / Market-by-Order Support

Extend the system to maintain individual order IDs and order-level state.

This would require tracking:

```text
Order ID
    │
    ├── Price
    ├── Quantity
    └── Side
```

and handling order additions, modifications, and cancellations at the individual-order level.

## Build Instructions

### Dependencies

The project requires:

- C++17 compatible compiler
  - GCC 9+
  - Clang 10+
- Boost
  - Asio
  - Beast
  - Algorithm
- OpenSSL
- simdjson
- CMake 3.15+

The build uses CMake `FetchContent` to automatically download and compile `simdjson`.

No manual installation of `simdjson` is required.

### Building

Clone the repository:

```bash
git clone https://github.com/gautamkr62/orderbook_reconstructor.git
cd orderbook_reconstructor
```

Create a release build:

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

### Basic Run

```bash
./orderbook_demo DOGEUSDT
```

### Specify Symbol

```bash
./orderbook_demo BTCUSDT
```

The implementation is symbol-agnostic and accepts Binance spot trading symbols.

### Stop Gracefully

Press:

```text
Ctrl+C
```

The application handles `SIGINT` and performs a clean shutdown:

```text
SIGINT
  │
  ▼
Stop IO
  │
  ▼
Stop Engine
  │
  ▼
Stop Validator
  │
  ▼
Join Threads
  │
  ▼
Close WebSocket
  │
  ▼
Exit
```

## Project Architecture

A simplified logical organization is:

```text
Order Book Reconstructor
│
├── Network Layer
│   ├── WebSocketClient
│   └── RestClient
│
├── Processing Layer
│   ├── Reconciler
│   ├── OrderBook
│   └── SideBook
│
├── History
│   └── DiffHistoryBuffer
│
├── Validation
│   └── Validator
│
├── Infrastructure
│   ├── ThreadSafeQueue
│   └── MetricsCollector
│
└── Application
    └── orderbook_demo
```

## Correctness Model

The system is designed around three independent correctness mechanisms.

### 1. Sequence Validation

WebSocket update IDs are checked continuously.

```text
Expected sequence
       │
       ▼
Incoming [U, u]
       │
       ├── Valid ──► Apply
       │
       └── Invalid ──► Resync
```

### 2. Deterministic Replay

Every processed delta is stored in the history buffer.

This makes it possible to reconstruct the state at a previous point from a known snapshot.

### 3. Independent Validation

The validator periodically performs an independent reconstruction using:

```text
REST snapshot
      +
historical WebSocket deltas
      =
shadow order book
```

The shadow state is then compared against the live state.

Together, these mechanisms prevent the system from relying solely on the correctness of its own live state.

## Technology Stack

| Technology | Purpose |
|---|---|
| **C++17** | Core implementation and low-latency processing |
| **Boost.Asio** | Asynchronous networking and IO |
| **Boost.Beast** | WebSocket and HTTP support |
| **OpenSSL** | TLS |
| **simdjson** | High-performance JSON parsing |
| **CMake** | Build system |
| **Binance WebSocket** | Real-time market-data deltas |
| **Binance REST API** | Order-book snapshots |

## Contributing

This is an open-source project. Feel free to open issues or submit pull requests.

Possible areas for contribution include:

- Multi-symbol support.
- Additional exchange integrations.
- Improved validation strategies.
- Performance optimization.
- Persistent metrics.
- L3 order-book support.
- Additional test coverage.
- Benchmarking and profiling.

## License

MIT License — see [LICENSE](LICENSE) for details.

## Acknowledgments

Built with:

- Boost.Beast
- Boost.Asio
- simdjson
- OpenSSL

Inspired by Binance's official WebSocket Market Streams documentation.
