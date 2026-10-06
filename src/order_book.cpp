#include "order_book.hpp"

#include <utility>

bool OrderBook::submit(Order&& order)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_closed) {
            return false;
        }
        _orders.push_back(std::move(order));
    }
    _ready.notify_one();
    return true;
}

std::optional<Order> OrderBook::take()
{
    std::unique_lock<std::mutex> lock(_mutex);
    // El predicado cubre los despertares espurios y el caso en que otro consumidor
    // se llevó el pedido primero: se vuelve a dormir si no hay nada que tomar.
    _ready.wait(lock, [this] { return !_orders.empty() || _closed; });

    if (_orders.empty()) {
        return std::nullopt;  // cerrado y vacío
    }
    std::optional<Order> order(std::move(_orders.front()));
    _orders.pop_front();
    return order;
}

void OrderBook::close()
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _closed = true;
    }
    _ready.notify_all();
}

std::size_t OrderBook::size() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _orders.size();
}
