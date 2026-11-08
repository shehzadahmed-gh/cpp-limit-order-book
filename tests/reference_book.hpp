#pragma once

// A deliberately naive order book used as a test oracle.
//
// Every operation is a linear scan over one flat vector of resting orders, so
// it is slow, but each rule (best price, then earliest arrival) is written out
// directly and is easy to check by eye. The randomized test in
// order_book_tests.cpp feeds the same command stream to this and to
// lob::OrderBook and requires identical trades, results and book state.

#include "order_book.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace lob::reference {

class ReferenceBook {
public:
    AddResult add_limit(OrderId id, Side side, Price price, Quantity quantity) {
        if (id == 0 || price <= 0 || quantity == 0 || find(id) != orders_.end()) {
            ++stats_.rejected_orders;
            return {false, "rejected", {}, quantity, false};
        }
        ++stats_.accepted_orders;
        return submit({id, side, price, quantity, next_priority_++}, false, true);
    }

    AddResult add_market(OrderId id, Side side, Quantity quantity) {
        if (id == 0 || quantity == 0 || find(id) != orders_.end()) {
            ++stats_.rejected_orders;
            return {false, "rejected", {}, quantity, false};
        }
        ++stats_.accepted_orders;
        return submit({id, side, 0, quantity, next_priority_++}, true, false);
    }

    bool cancel(OrderId id) {
        auto it = find(id);
        if (it == orders_.end()) {
            return false;
        }
        orders_.erase(it);
        ++stats_.cancelled_orders;
        return true;
    }

    ModifyResult modify(OrderId id, Price new_price, Quantity new_quantity) {
        auto it = find(id);
        if (it == orders_.end()) {
            return {ModificationEffect::NotFound, "not found", {}, 0, false};
        }
        if (new_price <= 0 || new_quantity == 0) {
            return {ModificationEffect::Rejected, "rejected", {}, 0, false};
        }
        if (new_price == it->price && new_quantity <= it->remaining) {
            it->remaining = new_quantity;
            ++stats_.modified_orders;
            return {ModificationEffect::InPlace, {}, {}, new_quantity, true};
        }
        const Side side = it->side;
        orders_.erase(it);
        AddResult r = submit({id, side, new_price, new_quantity, next_priority_++}, false, true);
        ++stats_.modified_orders;
        return {ModificationEffect::Requeued, {}, r.trades, r.unfilled_quantity, r.rested};
    }

    // Same shape as OrderBook::snapshot with unlimited depth.
    BookSnapshot snapshot() const {
        BookSnapshot out;
        out.bids = levels(Side::Buy);
        out.asks = levels(Side::Sell);
        return out;
    }

    std::optional<Price> price_of(OrderId id) const {
        for (const auto& o : orders_) {
            if (o.id == id) return o.price;
        }
        return std::nullopt;
    }

    const EngineStats& stats() const { return stats_; }
    std::size_t active_order_count() const { return orders_.size(); }

private:
    struct Resting {
        OrderId id;
        Side side;
        Price price;
        Quantity remaining;
        std::uint64_t priority;
    };

    std::vector<Resting>::iterator find(OrderId id) {
        return std::find_if(orders_.begin(), orders_.end(),
                            [id](const Resting& o) { return o.id == id; });
    }

    // Best resting order on the side opposite to `taker`: best price first,
    // then lowest priority number (earliest arrival).
    std::vector<Resting>::iterator best_maker(Side taker) {
        auto best = orders_.end();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->side == taker) {
                continue;
            }
            if (best == orders_.end()) {
                best = it;
                continue;
            }
            const bool better_price = taker == Side::Buy ? it->price < best->price
                                                         : it->price > best->price;
            if (better_price || (it->price == best->price && it->priority < best->priority)) {
                best = it;
            }
        }
        return best;
    }

    AddResult submit(Resting incoming, bool is_market, bool may_rest) {
        AddResult result;
        result.accepted = true;
        while (incoming.remaining > 0) {
            auto maker = best_maker(incoming.side);
            if (maker == orders_.end()) {
                break;
            }
            const bool crosses = is_market ||
                (incoming.side == Side::Buy ? incoming.price >= maker->price
                                            : incoming.price <= maker->price);
            if (!crosses) {
                break;
            }
            const Quantity qty = std::min(incoming.remaining, maker->remaining);
            result.trades.push_back(
                {next_trade_++, maker->id, incoming.id, incoming.side, maker->price, qty});
            incoming.remaining -= qty;
            maker->remaining -= qty;
            ++stats_.trades;
            stats_.matched_quantity += qty;
            if (maker->remaining == 0) {
                orders_.erase(maker);
            }
        }
        if (incoming.remaining > 0 && may_rest) {
            orders_.push_back(incoming);
            result.rested = true;
        }
        result.unfilled_quantity = incoming.remaining;
        return result;
    }

    std::vector<PriceLevelView> levels(Side side) const {
        std::vector<Resting> sorted;
        for (const auto& o : orders_) {
            if (o.side == side) {
                sorted.push_back(o);
            }
        }
        std::sort(sorted.begin(), sorted.end(), [side](const Resting& a, const Resting& b) {
            if (a.price != b.price) {
                return side == Side::Buy ? a.price > b.price : a.price < b.price;
            }
            return a.priority < b.priority;
        });
        std::vector<PriceLevelView> out;
        for (const auto& o : sorted) {
            if (out.empty() || out.back().price != o.price) {
                out.push_back({o.price, 0, {}});
            }
            out.back().total_quantity += o.remaining;
            out.back().orders.push_back({o.id, o.remaining, o.priority});
        }
        return out;
    }

    std::vector<Resting> orders_;
    std::uint64_t next_priority_{1};
    std::uint64_t next_trade_{1};
    EngineStats stats_;
};

} // namespace lob::reference
