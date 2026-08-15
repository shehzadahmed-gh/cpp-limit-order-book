#include "order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using lob::OrderBook;
using lob::OrderId;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::Side;

constexpr const char* kBlue = "\033[36m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kReset = "\033[0m";

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream input(line);
    std::string field;
    while (std::getline(input, field, ',')) {
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }
    return fields;
}

Side parse_side(const std::string& text) {
    if (text == "BUY") return Side::Buy;
    if (text == "SELL") return Side::Sell;
    throw std::invalid_argument("side must be BUY or SELL");
}

OrderType parse_type(const std::string& text) {
    if (text == "LIMIT") return OrderType::Limit;
    if (text == "MARKET") return OrderType::Market;
    throw std::invalid_argument("type must be LIMIT or MARKET");
}

std::string queue_text(const lob::PriceLevelView& level) {
    std::ostringstream out;
    for (std::size_t i = 0; i < level.orders.size(); ++i) {
        if (i != 0) out << " -> ";
        out << '#' << level.orders[i].id << ':' << level.orders[i].quantity;
    }
    return out.str();
}

void print_book(const OrderBook& book, std::size_t depth = 5) {
    const auto view = book.snapshot(depth);
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();

    std::cout << '\n' << kBold << "ORDER BOOK" << kReset << "  ";
    if (bid && ask) {
        std::cout << kDim << "spread " << lob::format_price(*ask - *bid)
                  << kReset;
    } else {
        std::cout << kDim << "one-sided" << kReset;
    }
    std::cout << "  active=" << book.active_order_count() << '\n';
    std::cout << kDim
              << "  SIDE      PRICE       QTY   FIFO QUEUE (order:remaining)"
              << kReset << '\n';

    for (auto level = view.asks.rbegin(); level != view.asks.rend(); ++level) {
        std::cout << "  " << kRed << "ASK " << kReset << "  "
                  << std::setw(10) << lob::format_price(level->price) << "  "
                  << std::setw(8) << level->total_quantity << "   "
                  << queue_text(*level) << '\n';
    }
    std::cout << "  " << kDim << "-----------------------------------------------"
              << kReset << '\n';
    for (const auto& level : view.bids) {
        std::cout << "  " << kGreen << "BID " << kReset << "  "
                  << std::setw(10) << lob::format_price(level.price) << "  "
                  << std::setw(8) << level.total_quantity << "   "
                  << queue_text(level) << '\n';
    }
}

void print_trades(const std::vector<lob::Trade>& trades) {
    for (const auto& trade : trades) {
        std::cout << "    " << kYellow << "TRADE " << std::setw(2)
                  << trade.sequence << kReset << "  "
                  << lob::to_string(trade.aggressor_side) << " taker #"
                  << trade.taker_id << " matched maker #" << trade.maker_id
                  << "  " << trade.quantity << " @ "
                  << lob::format_price(trade.price) << '\n';
    }
}

void require_consistent(const OrderBook& book) {
    std::string error;
    if (!book.verify(&error)) {
        throw std::runtime_error("book invariant failed: " + error);
    }
}

void show_add(const std::string& label, const lob::AddResult& result,
              const OrderBook& book) {
    std::cout << '\n' << kBlue << "> " << kReset << label << '\n';
    if (!result.accepted) {
        std::cout << "    " << kRed << "REJECTED" << kReset << "  "
                  << result.rejection << '\n';
    }
    print_trades(result.trades);
    if (result.rested) {
        std::cout << "    " << kDim << result.unfilled_quantity
                  << " resting on book" << kReset << '\n';
    } else if (result.unfilled_quantity > 0) {
        std::cout << "    " << kDim << result.unfilled_quantity
                  << " unfilled (market remainder discarded)" << kReset << '\n';
    }
    require_consistent(book);
    print_book(book);
}

void run_demo() {
    OrderBook book;
    std::cout << kBold << "C++ LIMIT ORDER BOOK / LIVE MATCHING DEMO" << kReset
              << '\n'
              << kDim
              << "Integer ticks | price-time priority | invariant check after every event"
              << kReset << '\n';

    show_add("NEW #1001 BUY  50 @ 100.00",
             book.add_limit(1001, Side::Buy, 10000, 50), book);
    show_add("NEW #1002 BUY  30 @ 100.00  (queues behind #1001)",
             book.add_limit(1002, Side::Buy, 10000, 30), book);
    show_add("NEW #2001 SELL 40 @ 100.05",
             book.add_limit(2001, Side::Sell, 10005, 40), book);
    show_add("NEW #2002 SELL 50 @ 100.10",
             book.add_limit(2002, Side::Sell, 10010, 50), book);
    show_add("NEW #2003 SELL 25 @ 100.10  (queues behind #2002)",
             book.add_limit(2003, Side::Sell, 10010, 25), book);

    std::cout << '\n' << kBlue << "> " << kReset
              << "MODIFY #2002 to 60 @ 100.10" << '\n';
    auto modified = book.modify(2002, 10010, 60);
    std::cout << "    " << kDim
              << "size increase loses priority: #2002 moves behind #2003"
              << kReset << '\n';
    print_trades(modified.trades);
    require_consistent(book);
    print_book(book);

    show_add("MARKET #9001 SELL 60  (FIFO consumes #1001 before #1002)",
             book.add_market(9001, Side::Sell, 60), book);
    show_add("LIMIT #9002 BUY 65 @ 100.10  (walks two ask levels)",
             book.add_limit(9002, Side::Buy, 10010, 65), book);

    std::cout << '\n' << kBlue << "> " << kReset << "CANCEL #2002" << '\n';
    const bool cancelled = book.cancel(2002);
    std::cout << "    " << (cancelled ? kGreen : kRed)
              << (cancelled ? "accepted" : "not found") << kReset << '\n';
    require_consistent(book);
    print_book(book);

    const auto& stats = book.stats();
    std::cout << '\n' << kBold << "SESSION SUMMARY" << kReset << '\n'
              << "  accepted orders : " << stats.accepted_orders << '\n'
              << "  modifications   : " << stats.modified_orders << '\n'
              << "  cancellations   : " << stats.cancelled_orders << '\n'
              << "  trades           : " << stats.trades << '\n'
              << "  matched quantity : " << stats.matched_quantity << '\n'
              << "  consistency      : " << kGreen << "PASS" << kReset << '\n';
}

void run_replay(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open replay file: " + path);
    }

    OrderBook book;
    std::string line;
    std::size_t line_number = 0;
    std::size_t events = 0;
    std::cout << kBold << "MARKET-DATA REPLAY" << kReset << "  " << path << '\n';

    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') continue;
        if (line_number == 1 && line.rfind("timestamp,", 0) == 0) continue;

        try {
            const auto fields = split_csv(line);
            if (fields.size() != 7) {
                throw std::invalid_argument("expected 7 CSV fields");
            }
            const auto id = static_cast<OrderId>(std::stoull(fields[2]));
            std::cout << '\n' << kBlue << fields[0] << "  " << kReset;

            if (fields[1] == "NEW") {
                const Side side = parse_side(fields[3]);
                const OrderType type = parse_type(fields[4]);
                const Quantity quantity = std::stoull(fields[6]);
                lob::AddResult result;
                if (type == OrderType::Limit) {
                    const Price price = lob::parse_price(fields[5]);
                    std::cout << "NEW #" << id << ' ' << fields[3] << ' '
                              << quantity << " @ " << fields[5] << '\n';
                    result = book.add_limit(id, side, price, quantity);
                } else {
                    std::cout << "NEW #" << id << ' ' << fields[3]
                              << " MARKET " << quantity << '\n';
                    result = book.add_market(id, side, quantity);
                }
                if (!result.accepted) {
                    std::cout << "    " << kRed << "REJECTED: "
                              << result.rejection << kReset << '\n';
                }
                print_trades(result.trades);
            } else if (fields[1] == "CANCEL") {
                std::cout << "CANCEL #" << id << '\n';
                std::cout << "    " << (book.cancel(id) ? "accepted" : "not found")
                          << '\n';
            } else if (fields[1] == "MODIFY") {
                const Price price = lob::parse_price(fields[5]);
                const Quantity quantity = std::stoull(fields[6]);
                std::cout << "MODIFY #" << id << " -> " << quantity << " @ "
                          << fields[5] << '\n';
                const auto result = book.modify(id, price, quantity);
                if (result.effect == lob::ModificationEffect::NotFound ||
                    result.effect == lob::ModificationEffect::Rejected) {
                    std::cout << "    " << kRed << result.rejection << kReset
                              << '\n';
                } else {
                    std::cout << "    " << kDim
                              << (result.effect == lob::ModificationEffect::InPlace
                                      ? "priority preserved"
                                      : "cancel/re-enter; new priority")
                              << kReset << '\n';
                }
                print_trades(result.trades);
            } else {
                throw std::invalid_argument("unknown action: " + fields[1]);
            }

            ++events;
            require_consistent(book);
            print_book(book);
        } catch (const std::exception& error) {
            throw std::runtime_error("CSV line " + std::to_string(line_number) +
                                     ": " + error.what());
        }
    }

    std::cout << '\n' << kGreen << "REPLAY PASS" << kReset << "  " << events
              << " events, " << book.stats().trades << " trades, "
              << book.active_order_count() << " active orders" << '\n';
}

void run_benchmark(std::size_t event_count) {
    if (event_count == 0) {
        throw std::invalid_argument("benchmark event count must be positive");
    }

    OrderBook book;
    std::mt19937_64 random(0xC0FFEE);
    std::vector<OrderId> candidates;
    candidates.reserve(event_count);
    std::vector<std::uint64_t> latencies;
    latencies.reserve(event_count);

    using Clock = std::chrono::steady_clock;
    const auto wall_start = Clock::now();
    for (std::size_t i = 1; i <= event_count; ++i) {
        const OrderId id = i;
        const Side side = (random() & 1U) ? Side::Buy : Side::Sell;
        const Quantity quantity = 1 + random() % 100;
        const unsigned action = random() % 100;
        const auto operation_start = Clock::now();

        if (action < 72) {
            // Passive prices build depth; occasional inside prices trade.
            const Price price = side == Side::Buy
                ? 9990 + static_cast<Price>(random() % 14)
                : 10010 - static_cast<Price>(random() % 14);
            const auto result = book.add_limit(id, side, price, quantity);
            if (result.rested) candidates.push_back(id);
        } else if (action < 86) {
            book.add_market(id, side, quantity);
        } else if (!candidates.empty() && action < 95) {
            const OrderId target = candidates[random() % candidates.size()];
            book.cancel(target);
        } else if (!candidates.empty()) {
            const OrderId target = candidates[random() % candidates.size()];
            const Price price = side == Side::Buy
                ? 9990 + static_cast<Price>(random() % 14)
                : 10010 - static_cast<Price>(random() % 14);
            book.modify(target, price, quantity);
        } else {
            book.add_market(id, side, quantity);
        }

        const auto operation_end = Clock::now();
        latencies.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                operation_end - operation_start)
                .count()));
    }
    const auto wall_end = Clock::now();

    require_consistent(book);
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double p) {
        const auto index = static_cast<std::size_t>(p * (latencies.size() - 1));
        return latencies[index];
    };
    const double seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    const double throughput = static_cast<double>(event_count) / seconds;

    std::cout << kBold << "DETERMINISTIC ENGINE BENCHMARK" << kReset << '\n'
              << "  events        : " << event_count << '\n'
              << "  elapsed       : " << std::fixed << std::setprecision(3)
              << seconds << " s" << '\n'
              << "  throughput    : " << std::setprecision(0) << throughput
              << " events/s" << '\n'
              << "  p50 latency   : " << percentile(0.50) << " ns" << '\n'
              << "  p95 latency   : " << percentile(0.95) << " ns" << '\n'
              << "  p99 latency   : " << percentile(0.99) << " ns" << '\n'
              << "  trades        : " << book.stats().trades << '\n'
              << "  active orders : " << book.active_order_count() << '\n'
              << "  consistency   : " << kGreen << "PASS" << kReset << '\n'
              << kDim
              << "  Note: single-thread, end-to-end command latency; build with -O3."
              << kReset << '\n';
}

void print_help(const char* executable) {
    std::cout << "Usage:\n"
              << "  " << executable << " --demo\n"
              << "  " << executable << " --replay <feed.csv>\n"
              << "  " << executable << " --benchmark [events]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1 || std::string(argv[1]) == "--demo") {
            run_demo();
        } else if (std::string(argv[1]) == "--replay" && argc == 3) {
            run_replay(argv[2]);
        } else if (std::string(argv[1]) == "--benchmark") {
            const std::size_t events = argc >= 3 ? std::stoull(argv[2]) : 250000;
            run_benchmark(events);
        } else if (std::string(argv[1]) == "--help" ||
                   std::string(argv[1]) == "-h") {
            print_help(argv[0]);
        } else {
            print_help(argv[0]);
            return 2;
        }
    } catch (const std::exception& error) {
        std::cerr << kRed << "error: " << error.what() << kReset << '\n';
        return 1;
    }
    return 0;
}
