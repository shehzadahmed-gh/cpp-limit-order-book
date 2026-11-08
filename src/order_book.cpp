#include "order_book.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace lob {

std::string to_string(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

std::string to_string(OrderType type) {
    return type == OrderType::Limit ? "LIMIT" : "MARKET";
}

Price parse_price(const std::string& text) {
    if (text.empty()) {
        throw std::invalid_argument("price is empty");
    }

    std::size_t i = 0;
    bool negative = false;
    if (text[i] == '+' || text[i] == '-') {
        negative = text[i] == '-';
        ++i;
    }
    if (i == text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) {
        throw std::invalid_argument("invalid price: " + text);
    }

    Price whole = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        const int digit = text[i++] - '0';
        if (whole > (std::numeric_limits<Price>::max() - digit) / 10) {
            throw std::out_of_range("price is too large");
        }
        whole = whole * 10 + digit;
    }

    Price fraction = 0;
    int fraction_digits = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            if (fraction_digits >= 2) {
                throw std::invalid_argument("price must use at most two decimals: " + text);
            }
            fraction = fraction * 10 + (text[i++] - '0');
            ++fraction_digits;
        }
    }
    if (i != text.size()) {
        throw std::invalid_argument("invalid price: " + text);
    }
    if (fraction_digits == 1) {
        fraction *= 10;
    }
    if (whole > (std::numeric_limits<Price>::max() - fraction) / 100) {
        throw std::out_of_range("price is too large");
    }

    const Price ticks = whole * 100 + fraction;
    return negative ? -ticks : ticks;
}

std::string format_price(Price price) {
    const bool negative = price < 0;
    const auto magnitude = negative
        ? static_cast<std::uint64_t>(-(price + 1)) + 1U
        : static_cast<std::uint64_t>(price);
    std::ostringstream out;
    if (negative) {
        out << '-';
    }
    out << magnitude / 100 << '.' << std::setw(2) << std::setfill('0')
        << magnitude % 100;
    return out.str();
}

AddResult OrderBook::add_limit(OrderId id, Side side, Price price,
                               Quantity quantity) {
    if (id == 0 || price <= 0 || quantity == 0 || index_.contains(id)) {
        ++stats_.rejected_orders;
        return {false,
                index_.contains(id) ? "duplicate active order id"
                                    : "id, price and quantity must be positive",
                {}, quantity, false};
    }

    Order incoming{id, side, price, quantity, next_priority_++};
    return submit(incoming, true, true);
}

AddResult OrderBook::add_market(OrderId id, Side side, Quantity quantity) {
    if (id == 0 || quantity == 0 || index_.contains(id)) {
        ++stats_.rejected_orders;
        return {false,
                index_.contains(id) ? "duplicate active order id"
                                    : "id and quantity must be positive",
                {}, quantity, false};
    }

    const Price sentinel = side == Side::Buy
        ? std::numeric_limits<Price>::max()
        : std::numeric_limits<Price>::min();
    Order incoming{id, side, sentinel, quantity, next_priority_++};
    return submit(incoming, false, true);
}

AddResult OrderBook::submit(Order incoming, bool may_rest, bool count_as_new) {
    AddResult result;
    result.accepted = true;
    if (count_as_new) {
        ++stats_.accepted_orders;
    }

    while (incoming.remaining > 0 && crosses(incoming)) {
        Queue& queue = incoming.side == Side::Buy ? asks_.begin()->second
                                                  : bids_.begin()->second;
        Order& maker = queue.front();
        const Quantity execution = std::min(incoming.remaining, maker.remaining);
        result.trades.push_back(make_trade(maker, incoming, execution));
        incoming.remaining -= execution;
        maker.remaining -= execution;

        ++stats_.trades;
        stats_.matched_quantity += execution;

        if (maker.remaining == 0) {
            index_.erase(maker.id);
            queue.pop_front();
            if (queue.empty()) {
                if (incoming.side == Side::Buy) {
                    asks_.erase(asks_.begin());
                } else {
                    bids_.erase(bids_.begin());
                }
            }
        }
    }

    if (incoming.remaining > 0 && may_rest) {
        rest(incoming);
        result.rested = true;
    }
    result.unfilled_quantity = incoming.remaining;
    return result;
}

bool OrderBook::crosses(const Order& incoming) const {
    if (incoming.side == Side::Buy) {
        return !asks_.empty() && incoming.price >= asks_.begin()->first;
    }
    return !bids_.empty() && incoming.price <= bids_.begin()->first;
}

Trade OrderBook::make_trade(const Order& maker, const Order& taker,
                            Quantity quantity) {
    return {next_trade_sequence_++, maker.id, taker.id, taker.side, maker.price,
            quantity};
}

void OrderBook::rest(Order order) {
    if (order.side == Side::Buy) {
        auto [level, _] = bids_.try_emplace(order.price);
        level->second.push_back(order);
        auto position = std::prev(level->second.end());
        index_.emplace(order.id,
                       Locator{Side::Buy, order.price, position});
    } else {
        auto [level, _] = asks_.try_emplace(order.price);
        level->second.push_back(order);
        auto position = std::prev(level->second.end());
        index_.emplace(order.id,
                       Locator{Side::Sell, order.price, position});
    }
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) {
        return false;
    }
    erase_resting(found);
    ++stats_.cancelled_orders;
    return true;
}

void OrderBook::erase_resting(
    std::unordered_map<OrderId, Locator>::iterator found) {
    const Side side = found->second.side;
    const Price price = found->second.price;
    const auto position = found->second.position;
    index_.erase(found);

    if (side == Side::Buy) {
        auto level = bids_.find(price);
        level->second.erase(position);
        if (level->second.empty()) {
            bids_.erase(level);
        }
    } else {
        auto level = asks_.find(price);
        level->second.erase(position);
        if (level->second.empty()) {
            asks_.erase(level);
        }
    }
}

ModifyResult OrderBook::modify(OrderId id, Price new_price,
                               Quantity new_quantity) {
    auto found = index_.find(id);
    if (found == index_.end()) {
        return {ModificationEffect::NotFound, "order is not active", {}, 0,
                false};
    }
    if (new_price <= 0 || new_quantity == 0) {
        return {ModificationEffect::Rejected,
                "price and quantity must be positive", {}, 0, false};
    }

    Order& existing = *found->second.position;
    if (new_price == existing.price && new_quantity <= existing.remaining) {
        existing.remaining = new_quantity;
        ++stats_.modified_orders;
        return {ModificationEffect::InPlace, {}, {}, new_quantity, true};
    }

    const Side side = existing.side;
    erase_resting(found);
    Order replacement{id, side, new_price, new_quantity, next_priority_++};
    AddResult submitted = submit(replacement, true, false);
    ++stats_.modified_orders;
    return {ModificationEffect::Requeued, {}, std::move(submitted.trades),
            submitted.unfilled_quantity, submitted.rested};
}

std::optional<Price> OrderBook::best_bid() const {
    return bids_.empty() ? std::nullopt
                         : std::optional<Price>{bids_.begin()->first};
}

std::optional<Price> OrderBook::best_ask() const {
    return asks_.empty() ? std::nullopt
                         : std::optional<Price>{asks_.begin()->first};
}

template <typename Levels>
std::vector<PriceLevelView> OrderBook::levels_view(const Levels& levels,
                                                   std::size_t depth) {
    std::vector<PriceLevelView> result;
    result.reserve(std::min(depth, levels.size()));
    for (const auto& [price, queue] : levels) {
        if (result.size() == depth) {
            break;
        }
        PriceLevelView view;
        view.price = price;
        for (const auto& order : queue) {
            view.total_quantity += order.remaining;
            view.orders.push_back(
                OrderView{order.id, order.remaining, order.priority});
        }
        result.push_back(std::move(view));
    }
    return result;
}

BookSnapshot OrderBook::snapshot(std::size_t depth) const {
    return {levels_view(bids_, depth), levels_view(asks_, depth)};
}

bool OrderBook::verify(std::string* error) const {
    auto fail = [&](const std::string& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    std::unordered_set<OrderId> seen;
    std::size_t count = 0;
    auto inspect = [&](const auto& levels, Side expected_side) {
        for (const auto& [price, queue] : levels) {
            if (queue.empty()) {
                return fail("empty price level at " + format_price(price));
            }
            std::uint64_t previous_priority = 0;
            for (const auto& order : queue) {
                ++count;
                if (order.side != expected_side || order.price != price ||
                    order.remaining == 0) {
                    return fail("order/level fields are inconsistent");
                }
                if (order.priority <= previous_priority) {
                    return fail("FIFO priority is not strictly increasing");
                }
                previous_priority = order.priority;
                if (!seen.insert(order.id).second) {
                    return fail("duplicate active order id");
                }
                auto indexed = index_.find(order.id);
                if (indexed == index_.end() ||
                    indexed->second.side != expected_side ||
                    indexed->second.price != price ||
                    &*indexed->second.position != &order) {
                    return fail("order index points to the wrong queue entry");
                }
            }
        }
        return true;
    };

    if (!inspect(bids_, Side::Buy) || !inspect(asks_, Side::Sell)) {
        return false;
    }
    if (count != index_.size()) {
        return fail("order index size does not match book contents");
    }
    if (!bids_.empty() && !asks_.empty() &&
        bids_.begin()->first >= asks_.begin()->first) {
        return fail("book is crossed after event processing");
    }
    return true;
}

} // namespace lob
