// message.cpp — Message クラス実装
#include "pipeutil/message.hpp"
#include <cstring>

namespace pipeutil {

Message::Message(std::span<const std::byte> data)
    : data_(data.begin(), data.end())
{}

Message::Message(std::string_view text)
    : data_(reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data()) + text.size())
{}

std::span<const std::byte> Message::payload() const noexcept {
    return {data_.data(), data_.size()};
}

std::size_t Message::size() const noexcept {
    return data_.size();
}

bool Message::empty() const noexcept {
    return data_.empty();
}

std::string_view Message::as_string_view() const noexcept {
    return {reinterpret_cast<const char*>(data_.data()), data_.size()};
}

} // namespace pipeutil
