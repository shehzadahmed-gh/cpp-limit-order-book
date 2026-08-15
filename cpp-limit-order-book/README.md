# C++ Limit Order Book & Matching Engine

A runnable portfolio demo of a single-instrument matching engine. It implements
price-time priority for limit and market orders, constant-time active-order
lookup, cancellation and exchange-style modification semantics. A deterministic
CSV replay, invariant checker, tests and microbenchmark make the behavior easy
to inspect rather than leaving it as a code-only claim.

## Quick start

Requirements: a C++20 compiler and `make`.

```bash
make -j
make demo
make replay
make test
make benchmark
```

Or use CMake on systems where it is available:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The demo prints every event, resulting trade and the post-event FIFO queues:

```text
ORDER BOOK  spread 0.05  active=5
  SIDE      PRICE       QTY   FIFO QUEUE (order:remaining)
  ASK       100.10        75   #2002:50 -> #2003:25
  ASK       100.05        40   #2001:40
  -----------------------------------------------
  BID       100.00        80   #1001:50 -> #1002:30
```

## Matching behavior

- **Best price first.** The lowest ask matches buys; the highest bid matches
  sells.
- **FIFO within a price.** Each price level is a list, so maker orders execute
  in arrival order.
- **Maker-price execution.** Every trade is priced at the resting order.
- **Limit remainder rests.** Any unfilled quantity is queued at its limit.
- **Market remainder expires.** It never becomes a resting order.
- **Priority-aware modification.** Reducing quantity at the same price keeps
  priority. Increasing size or changing price cancels and re-enters the order.
- **Integer prices.** The engine stores cents as integer ticks and never uses
  floating-point values in matching logic.

## Data structures

| Concern | Structure | Cost |
| --- | --- | --- |
| Bid levels | `std::map<Price, Queue, std::greater<>>` | best bid O(1), new level O(log P) |
| Ask levels | `std::map<Price, Queue>` | best ask O(1), new level O(log P) |
| FIFO at a level | `std::list<Order>` | append/erase O(1) |
| Order lookup | `std::unordered_map<OrderId, Locator>` | average O(1) cancel/modify |
| Match | best opposite level + front of queue | O(fills + removed levels log P) |

`P` is the number of active price levels. The lookup table stores stable list
iterators, so a cancellation does not scan the book.

## Commands

```bash
./build/lob_demo --demo
./build/lob_demo --replay data/sample_feed.csv
./build/lob_demo --benchmark 1000000
./build/lob_tests
```

The benchmark uses a fixed RNG seed and a mixed stream of limit orders, market
orders, cancels and modifications. It reports total throughput and p50/p95/p99
command latency. Results are machine-specific; they are intended for relative
scaling experiments, not exchange-grade latency claims.

## Replay format

The CSV schema is:

```text
timestamp,action,id,side,type,price,quantity
09:30:00.001,NEW,101,BUY,LIMIT,100.00,50
09:30:00.002,NEW,901,SELL,MARKET,,20
09:30:00.003,MODIFY,101,,,99.95,40
09:30:00.004,CANCEL,101,,,,
```

The replay validates the entire book after every input event: order/index
agreement, unique IDs, non-empty levels, FIFO priority, positive remaining
quantity and an uncrossed post-event book. Any violation stops replay with a
diagnostic.

## Project layout

```text
include/order_book.hpp       Public engine API and result types
src/order_book.cpp           Matching, cancellation and modification logic
src/main.cpp                 Live demo, CSV replay and benchmark CLI
tests/order_book_tests.cpp   Behavioral and invariant tests
data/sample_feed.csv         Deterministic sample market-data stream
```

## Scope

This is deliberately a clear single-threaded reference engine. Production
extensions would typically add allocation-free message pools, instrument
partitioning, persistence/recovery, self-trade prevention, richer order types,
binary feed decoding and concurrency at the gateway/partition boundary.
