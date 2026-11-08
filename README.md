# C++ Limit Order Book and Matching Engine

[![CI](https://github.com/shehzadahmed-gh/cpp-limit-order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/shehzadahmed-gh/cpp-limit-order-book/actions/workflows/ci.yml)

A single-threaded limit order book in C++20. It matches limit and market orders by price-time priority, cancels and modifies resting orders in constant time, and checks its own invariants after every event when replaying a feed. Prices are stored as integer ticks, so the matching path never touches floating point.

## Quick start

You need a C++20 compiler and either `make` or CMake.

```bash
make -j
make demo        # scripted walkthrough, prints the book after every event
make replay      # replays data/sample_feed.csv with invariant checks
make test        # unit tests + randomized test against a reference book
make sanitize    # same tests under AddressSanitizer and UBSan
make benchmark   # 1M mixed commands, reports throughput and latency percentiles
```

With CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Pass `-DLOB_SANITIZE=ON` to build the CMake targets with sanitizers.

Part of the demo output, after order #2002 is increased from 50 to 60 and loses its place behind #2003:

```text
ORDER BOOK  spread 0.05  active=5
  SIDE      PRICE       QTY   FIFO QUEUE (order:remaining)
  ASK       100.10        85   #2003:25 -> #2002:60
  ASK       100.05        40   #2001:40
  -----------------------------------------------
  BID       100.00        80   #1001:50 -> #1002:30
```

## Matching rules

- The best price matches first: the lowest ask for an incoming buy, the highest bid for an incoming sell.
- Orders at the same price fill in arrival order.
- Every trade prints at the resting (maker) order's price.
- Any unfilled part of a limit order rests at its limit price. An unfilled market order is discarded.
- Reducing an order's size at the same price keeps its queue position. Changing the price or increasing the size cancels it and re-enters it at the back of the queue, and a re-entered order can trade immediately if it now crosses.
- An order ID must be unique among active orders. ID 0, non-positive prices and zero quantities are rejected.

## Data structures

| Concern | Structure | Cost |
| --- | --- | --- |
| Bid levels | `std::map<Price, Queue, std::greater<>>` | best bid O(1), new level O(log P) |
| Ask levels | `std::map<Price, Queue>` | best ask O(1), new level O(log P) |
| Queue at one price | `std::list<Order>` | append and erase O(1) |
| Order lookup | `std::unordered_map<OrderId, Locator>` | average O(1) cancel and modify |
| Matching | best opposite level, front of its queue | O(fills + emptied levels × log P) |

`P` is the number of active price levels. The lookup table stores a list iterator for each resting order, so a cancel goes straight to the order without scanning its price level.

## Testing

`tests/order_book_tests.cpp` has hand-written cases for each rule above plus price parsing edge cases (including overflow when a price is scaled to ticks).

The main test is a randomized comparison against `tests/reference_book.hpp`, a deliberately naive book that keeps every resting order in one vector and finds the best maker with a linear scan. Both books receive the same 100,000 random commands (25 seeds × 4,000), including invalid IDs, prices and quantities, duplicate IDs, cancels of unknown orders, in-place reductions and crossing modifications. After every command the test requires the same trades, results, full book contents (price, quantity, queue order and priority of every order) and statistics, and runs `verify()` on the engine.

To check that the test has teeth, I planted bugs in the engine. Allowing an in-place modify for a size increase of 1 passes all of the hand-written tests, but the randomized test catches it at seed 4, step 541.

CI builds with GCC and Clang on every push and runs the suite again under AddressSanitizer and UBSan.

## Benchmark

`--benchmark N` sends a fixed-seed stream of N commands (72% limit orders, 14% market orders, the rest cancels and modifies) and times each command with `steady_clock`. Three runs of `make benchmark` on a Linux cloud VM (GCC 13, `-O3`) gave:

| Throughput | p50 | p95 | p99 |
| --- | --- | --- | --- |
| 2.1 to 2.3 M commands/s | 213 to 222 ns | about 1.0 µs | about 1.7 µs |

These numbers are machine specific and the per-command timing includes two clock reads. The stream ends with about 280,000 resting orders packed into a band of 21 price ticks, which is far deeper than a real book, so treat the numbers as a baseline for comparing changes.

## Replay format

```text
timestamp,action,id,side,type,price,quantity
09:30:00.001,NEW,101,BUY,LIMIT,100.00,50
09:30:00.002,NEW,901,SELL,MARKET,,20
09:30:00.003,MODIFY,101,,,99.95,40
09:30:00.004,CANCEL,101,,,,
```

After each line the replay checks that the index and the book agree, IDs are unique, no level is empty, priorities increase through each queue, every resting order has a positive quantity and the book is not crossed. The first violation stops the replay with the line number.

## Layout

```text
include/order_book.hpp       public API and result types
src/order_book.cpp           matching, cancel and modify
src/main.cpp                 demo, CSV replay and benchmark CLI
tests/order_book_tests.cpp   unit tests and randomized reference test
tests/reference_book.hpp     naive reference book used as the test oracle
data/sample_feed.csv         sample command stream for the replay
```

## Limitations and next steps

The engine handles one instrument on one thread. A production engine would also need self-trade prevention, more order types (IOC, FOK, stop), persistence and recovery, and a binary feed decoder. The biggest cost in the current design is pointer chasing: every resting order is a separately allocated list node, and the order index is a node-based hash map. A flat order pool with intrusive links would be the next thing to try.
