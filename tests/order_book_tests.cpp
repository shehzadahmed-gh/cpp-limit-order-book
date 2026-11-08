#include "order_book.hpp"
#include "reference_book.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using lob::ModificationEffect;
using lob::OrderBook;
using lob::Side;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                      \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__      \
                      << ": " #condition << '\n';                              \
            std::exit(1);                                                        \
        }                                                                        \
    } while (false)

void fifo_at_same_price() {
    OrderBook book;
    book.add_limit(1, Side::Sell, 10000, 10);
    book.add_limit(2, Side::Sell, 10000, 10);
    const auto result = book.add_market(3, Side::Buy, 15);
    CHECK(result.trades.size() == 2);
    CHECK(result.trades[0].maker_id == 1);
    CHECK(result.trades[0].quantity == 10);
    CHECK(result.trades[1].maker_id == 2);
    CHECK(result.trades[1].quantity == 5);
}

void best_price_wins() {
    OrderBook book;
    book.add_limit(1, Side::Sell, 10100, 10);
    book.add_limit(2, Side::Sell, 10000, 10);
    const auto result = book.add_market(3, Side::Buy, 5);
    CHECK(result.trades.size() == 1);
    CHECK(result.trades[0].maker_id == 2);
    CHECK(result.trades[0].price == 10000);
}

void limit_remainder_rests() {
    OrderBook book;
    book.add_limit(1, Side::Sell, 10000, 7);
    const auto result = book.add_limit(2, Side::Buy, 10000, 10);
    CHECK(result.trades.size() == 1);
    CHECK(result.rested);
    CHECK(result.unfilled_quantity == 3);
    CHECK(book.best_bid() == 10000);
    CHECK(!book.best_ask());
}

void market_remainder_is_discarded() {
    OrderBook book;
    book.add_limit(1, Side::Sell, 10000, 7);
    const auto result = book.add_market(2, Side::Buy, 10);
    CHECK(!result.rested);
    CHECK(result.unfilled_quantity == 3);
    CHECK(book.active_order_count() == 0);
}

void cancel_removes_order_and_empty_level() {
    OrderBook book;
    book.add_limit(1, Side::Buy, 10000, 10);
    CHECK(book.cancel(1));
    CHECK(!book.cancel(1));
    CHECK(!book.best_bid());
    CHECK(book.bid_level_count() == 0);
}

void reduction_preserves_priority() {
    OrderBook book;
    book.add_limit(1, Side::Buy, 10000, 20);
    book.add_limit(2, Side::Buy, 10000, 20);
    const auto modify = book.modify(1, 10000, 10);
    CHECK(modify.effect == ModificationEffect::InPlace);
    const auto result = book.add_market(3, Side::Sell, 15);
    CHECK(result.trades.size() == 2);
    CHECK(result.trades[0].maker_id == 1);
    CHECK(result.trades[1].maker_id == 2);
}

void increase_loses_priority() {
    OrderBook book;
    book.add_limit(1, Side::Buy, 10000, 10);
    book.add_limit(2, Side::Buy, 10000, 10);
    const auto modify = book.modify(1, 10000, 20);
    CHECK(modify.effect == ModificationEffect::Requeued);
    const auto result = book.add_market(3, Side::Sell, 15);
    CHECK(result.trades.size() == 2);
    CHECK(result.trades[0].maker_id == 2);
    CHECK(result.trades[1].maker_id == 1);
}

void price_change_can_cross() {
    OrderBook book;
    book.add_limit(1, Side::Buy, 9900, 10);
    book.add_limit(2, Side::Sell, 10100, 8);
    const auto modify = book.modify(1, 10100, 10);
    CHECK(modify.effect == ModificationEffect::Requeued);
    CHECK(modify.trades.size() == 1);
    CHECK(modify.trades[0].maker_id == 2);
    CHECK(modify.unfilled_quantity == 2);
    CHECK(book.best_bid() == 10100);
}

void rejects_duplicate_active_id() {
    OrderBook book;
    CHECK(book.add_limit(1, Side::Buy, 10000, 10).accepted);
    CHECK(!book.add_limit(1, Side::Sell, 10100, 5).accepted);
    CHECK(book.active_order_count() == 1);
}

void snapshot_aggregates_levels() {
    OrderBook book;
    book.add_limit(1, Side::Buy, 10000, 10);
    book.add_limit(2, Side::Buy, 10000, 15);
    const auto view = book.snapshot();
    CHECK(view.bids.size() == 1);
    CHECK(view.bids[0].total_quantity == 25);
    CHECK(view.bids[0].orders.size() == 2);
}

void invariants_hold_after_mixed_stream() {
    OrderBook book;
    for (std::uint64_t i = 1; i <= 100; ++i) {
        const auto side = i % 2 == 0 ? Side::Buy : Side::Sell;
        const auto price = side == Side::Buy ? 9990 - static_cast<int>(i % 5)
                                             : 10010 + static_cast<int>(i % 5);
        book.add_limit(i, side, price, 10 + i);
    }
    book.add_market(1001, Side::Buy, 180);
    book.add_market(1002, Side::Sell, 220);
    book.cancel(42);
    book.modify(55, 10008, 7);
    std::string error;
    CHECK(book.verify(&error));
}

template <typename F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void price_parsing() {
    CHECK(lob::parse_price("100") == 10000);
    CHECK(lob::parse_price("100.5") == 10050);
    CHECK(lob::parse_price("100.05") == 10005);
    CHECK(lob::parse_price("0.01") == 1);
    CHECK(lob::format_price(10005) == "100.05");
    CHECK(throws([] { lob::parse_price(""); }));
    CHECK(throws([] { lob::parse_price("1.234"); }));
    CHECK(throws([] { lob::parse_price("12a"); }));
    // INT64_MAX ticks is the largest representable price; one more whole
    // unit fits in int64 on its own but overflows once scaled to ticks.
    CHECK(lob::parse_price("92233720368547758.07") == std::numeric_limits<lob::Price>::max());
    CHECK(throws([] { lob::parse_price("92233720368547758.08"); }));
    CHECK(throws([] { lob::parse_price("92233720368547759"); }));
}

// ---------------------------------------------------------------------------
// Randomized differential test against tests/reference_book.hpp.
// ---------------------------------------------------------------------------

bool same_trades(const std::vector<lob::Trade>& a, const std::vector<lob::Trade>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].sequence != b[i].sequence || a[i].maker_id != b[i].maker_id ||
            a[i].taker_id != b[i].taker_id || a[i].aggressor_side != b[i].aggressor_side ||
            a[i].price != b[i].price || a[i].quantity != b[i].quantity) {
            return false;
        }
    }
    return true;
}

bool same_levels(const std::vector<lob::PriceLevelView>& a,
                 const std::vector<lob::PriceLevelView>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].price != b[i].price || a[i].total_quantity != b[i].total_quantity ||
            a[i].orders.size() != b[i].orders.size()) {
            return false;
        }
        for (std::size_t j = 0; j < a[i].orders.size(); ++j) {
            const auto& x = a[i].orders[j];
            const auto& y = b[i].orders[j];
            if (x.id != y.id || x.quantity != y.quantity || x.priority != y.priority) {
                return false;
            }
        }
    }
    return true;
}

bool same_stats(const lob::EngineStats& a, const lob::EngineStats& b) {
    return a.accepted_orders == b.accepted_orders && a.rejected_orders == b.rejected_orders &&
           a.cancelled_orders == b.cancelled_orders && a.modified_orders == b.modified_orders &&
           a.trades == b.trades && a.matched_quantity == b.matched_quantity;
}

void run_against_reference(std::uint64_t seed, int steps) {
    OrderBook book;
    lob::reference::ReferenceBook ref;
    std::mt19937_64 rng(seed);
    std::vector<lob::OrderId> seen_ids;
    lob::OrderId next_id = 1;

    auto pick = [&](auto lo, auto hi) {
        return std::uniform_int_distribution<decltype(hi - lo)>(lo, hi)(rng);
    };
    auto fail = [&](int step, const char* what) {
        std::cerr << "reference mismatch (" << what << ") seed=" << seed << " step=" << step
                  << '\n';
        std::exit(1);
    };
    auto choose_id = [&]() -> lob::OrderId {
        // Mostly fresh ids, sometimes an id we've used before (active or not),
        // occasionally the invalid id 0.
        const int roll = pick(0, 99);
        if (roll < 2) return 0;
        if (roll < 10 && !seen_ids.empty()) return seen_ids[pick(std::size_t{0}, seen_ids.size() - 1)];
        seen_ids.push_back(next_id);
        return next_id++;
    };
    auto choose_existing = [&]() -> lob::OrderId {
        if (seen_ids.empty() || pick(0, 9) == 0) return 999'999'999;  // never issued
        return seen_ids[pick(std::size_t{0}, seen_ids.size() - 1)];
    };
    auto choose_price = [&]() -> lob::Price {
        if (pick(0, 199) == 0) return pick(-5, 0);  // invalid
        return 9'990 + pick(0, 20);                 // tight band so orders cross often
    };
    auto choose_qty = [&]() -> lob::Quantity {
        if (pick(0, 199) == 0) return 0;  // invalid
        return pick(1, 50);
    };

    for (int step = 0; step < steps; ++step) {
        const int action = pick(0, 99);
        const Side side = pick(0, 1) ? Side::Buy : Side::Sell;
        if (action < 50) {
            const auto id = choose_id();
            const auto price = choose_price();
            const auto qty = choose_qty();
            const auto a = book.add_limit(id, side, price, qty);
            const auto b = ref.add_limit(id, side, price, qty);
            if (a.accepted != b.accepted || a.rested != b.rested ||
                a.unfilled_quantity != b.unfilled_quantity || !same_trades(a.trades, b.trades)) {
                fail(step, "add_limit");
            }
        } else if (action < 60) {
            const auto id = choose_id();
            const auto qty = choose_qty();
            const auto a = book.add_market(id, side, qty);
            const auto b = ref.add_market(id, side, qty);
            if (a.accepted != b.accepted || a.rested != b.rested ||
                a.unfilled_quantity != b.unfilled_quantity || !same_trades(a.trades, b.trades)) {
                fail(step, "add_market");
            }
        } else if (action < 82) {
            const auto id = choose_existing();
            if (book.cancel(id) != ref.cancel(id)) fail(step, "cancel");
        } else {
            const auto id = choose_existing();
            const auto qty = choose_qty();
            auto price = choose_price();
            // Two times in three keep the current price, so the in-place
            // (priority-preserving) path is exercised as well as re-queues.
            if (pick(0, 2) != 0) {
                if (const auto current = ref.price_of(id)) price = *current;
            }
            const auto a = book.modify(id, price, qty);
            const auto b = ref.modify(id, price, qty);
            if (a.effect != b.effect || a.rested != b.rested ||
                a.unfilled_quantity != b.unfilled_quantity || !same_trades(a.trades, b.trades)) {
                fail(step, "modify");
            }
        }

        const auto mine = book.snapshot(std::numeric_limits<std::size_t>::max());
        const auto theirs = ref.snapshot();
        if (!same_levels(mine.bids, theirs.bids) || !same_levels(mine.asks, theirs.asks)) {
            fail(step, "book state");
        }
        if (book.active_order_count() != ref.active_order_count()) fail(step, "order count");
        if (!same_stats(book.stats(), ref.stats())) fail(step, "stats");
        std::string error;
        if (!book.verify(&error)) {
            std::cerr << "invariant failed: " << error << '\n';
            fail(step, "verify");
        }
    }
}

void matches_reference_model() {
    for (std::uint64_t seed = 1; seed <= 25; ++seed) {
        run_against_reference(seed, 4'000);
    }
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"FIFO at same price", fifo_at_same_price},
        {"best price wins", best_price_wins},
        {"limit remainder rests", limit_remainder_rests},
        {"market remainder discarded", market_remainder_is_discarded},
        {"cancel removes empty level", cancel_removes_order_and_empty_level},
        {"reduction preserves priority", reduction_preserves_priority},
        {"increase loses priority", increase_loses_priority},
        {"price change can cross", price_change_can_cross},
        {"duplicate active id rejected", rejects_duplicate_active_id},
        {"snapshot aggregates levels", snapshot_aggregates_levels},
        {"mixed-stream invariants", invariants_hold_after_mixed_stream},
        {"price parsing", price_parsing},
        {"matches reference model (25 seeds x 4000 ops)", matches_reference_model},
    };

    for (const auto& [name, test] : tests) {
        test();
        std::cout << "[PASS] " << name << '\n';
    }
    std::cout << tests.size() << "/" << tests.size() << " tests passed\n";
}
