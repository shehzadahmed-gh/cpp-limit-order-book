#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;      // Integer ticks (cents in the demo).
using Quantity = std::uint64_t;

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

std::string to_string(Side side);
std::string to_string(OrderType type);
Price parse_price(const std::string& text);
std::string format_price(Price price);

struct Trade {
    std::uint64_t sequence{};
    OrderId maker_id{};
    OrderId taker_id{};
    Side aggressor_side{};
    Price price{};
    Quantity quantity{};
};

struct AddResult {
    bool accepted{false};
    std::string rejection;
    std::vector<Trade> trades;
    Quantity unfilled_quantity{};
    bool rested{false};
};

enum class ModificationEffect { NotFound, Rejected, InPlace, Requeued };

struct ModifyResult {
    ModificationEffect effect{ModificationEffect::NotFound};
    std::string rejection;
    std::vector<Trade> trades;
    Quantity unfilled_quantity{};
    bool rested{false};
};

struct OrderView {
    OrderId id{};
    Quantity quantity{};
    std::uint64_t priority{};
};

struct PriceLevelView {
    Price price{};
    Quantity total_quantity{};
    std::vector<OrderView> orders;
};

struct BookSnapshot {
    std::vector<PriceLevelView> bids; // Best to worst.
    std::vector<PriceLevelView> asks; // Best to worst.
};

struct EngineStats {
    std::uint64_t accepted_orders{};
    std::uint64_t rejected_orders{};
    std::uint64_t cancelled_orders{};
    std::uint64_t modified_orders{};
    std::uint64_t trades{};
    Quantity matched_quantity{};
};

class OrderBook {
public:
    AddResult add_limit(OrderId id, Side side, Price price, Quantity quantity);
    AddResult add_market(OrderId id, Side side, Quantity quantity);

    bool cancel(OrderId id);

    // A same-price quantity reduction preserves queue priority. Any price
    // change or size increase cancels and re-enters the order at the back.
    ModifyResult modify(OrderId id, Price new_price, Quantity new_quantity);

    [[nodiscard]] BookSnapshot snapshot(std::size_t depth = 5) const;
    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] std::size_t active_order_count() const { return index_.size(); }
    [[nodiscard]] std::size_t bid_level_count() const { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const { return asks_.size(); }
    [[nodiscard]] const EngineStats& stats() const { return stats_; }

    // Expensive diagnostic used by replay/tests, not the hot path.
    [[nodiscard]] bool verify(std::string* error = nullptr) const;

private:
    struct Order {
        OrderId id{};
        Side side{};
        Price price{};
        Quantity remaining{};
        std::uint64_t priority{};
    };

    using Queue = std::list<Order>;
    using Bids = std::map<Price, Queue, std::greater<Price>>;
    using Asks = std::map<Price, Queue>;

    struct Locator {
        Side side{};
        Price price{};
        Queue::iterator position;
    };

    AddResult submit(Order incoming, bool may_rest, bool count_as_new);
    void rest(Order order);
    void erase_resting(std::unordered_map<OrderId, Locator>::iterator found);
    bool crosses(const Order& incoming) const;
    Trade make_trade(const Order& maker, const Order& taker, Quantity quantity);

    template <typename Levels>
    static std::vector<PriceLevelView> levels_view(const Levels& levels,
                                                    std::size_t depth);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;
    std::uint64_t next_priority_{1};
    std::uint64_t next_trade_sequence_{1};
    EngineStats stats_;
};

} // namespace lob
