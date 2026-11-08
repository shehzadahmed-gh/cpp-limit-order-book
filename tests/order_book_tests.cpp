#include "order_book.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
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
    };

    for (const auto& [name, test] : tests) {
        test();
        std::cout << "[PASS] " << name << '\n';
    }
    std::cout << tests.size() << "/" << tests.size() << " tests passed\n";
}
