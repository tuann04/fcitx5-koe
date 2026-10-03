#ifndef KOE_COMMON_PROTOCOL_H
#define KOE_COMMON_PROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace koe {

struct Message {
    enum class Type { Start, Stop, Cancel, Text, Error, Partial };

    Type type = Type::Start;
    uint64_t id = 0;
    std::string payload;
};

namespace detail {

inline std::string escapePayload(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (char c : input) {
        switch (c) {
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        default:
            output += c;
            break;
        }
    }
    return output;
}

inline std::optional<std::string> unescapePayload(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];
        if (c != '\\') {
            output += c;
            continue;
        }
        if (++i >= input.size()) {
            return std::nullopt;
        }
        switch (input[i]) {
        case '\\':
            output += '\\';
            break;
        case 'n':
            output += '\n';
            break;
        case 'r':
            output += '\r';
            break;
        default:
            return std::nullopt;
        }
    }
    return output;
}

inline bool parseId(std::string_view text, uint64_t &id) {
    if (text.empty()) {
        return false;
    }
    uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    id = value;
    return true;
}

} // namespace detail

inline std::string format(const Message &message) {
    const std::string id = std::to_string(message.id);
    std::string line;
    switch (message.type) {
    case Message::Type::Start:
        line = "START " + id + " lang=" + message.payload;
        break;
    case Message::Type::Stop:
        line = "STOP " + id;
        break;
    case Message::Type::Cancel:
        line = "CANCEL " + id;
        break;
    case Message::Type::Text:
        line = "TEXT " + id + " " + detail::escapePayload(message.payload);
        break;
    case Message::Type::Error:
        line = "ERROR " + id + " " + detail::escapePayload(message.payload);
        break;
    case Message::Type::Partial:
        line = "PARTIAL " + id + " " + detail::escapePayload(message.payload);
        break;
    }
    line += '\n';
    return line;
}

inline std::optional<Message> parse(std::string_view line) {
    if (line.empty()) {
        return std::nullopt;
    }

    const size_t verbEnd = line.find(' ');
    if (verbEnd == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view verb = line.substr(0, verbEnd);
    std::string_view rest = line.substr(verbEnd + 1);

    const size_t idEnd = rest.find(' ');
    const std::string_view idText = rest.substr(0, idEnd);
    rest = idEnd == std::string_view::npos ? std::string_view{}
                                           : rest.substr(idEnd + 1);

    Message message;
    if (!detail::parseId(idText, message.id)) {
        return std::nullopt;
    }

    if (verb == "START") {
        constexpr std::string_view prefix = "lang=";
        if (rest.size() < prefix.size() || rest.substr(0, prefix.size()) != prefix) {
            return std::nullopt;
        }
        const std::string_view lang = rest.substr(prefix.size());
        if (lang.empty() || lang.size() > 16 ||
            lang.find(' ') != std::string_view::npos) {
            return std::nullopt;
        }
        message.type = Message::Type::Start;
        message.payload = std::string(lang);
        return message;
    }
    if (verb == "STOP") {
        if (!rest.empty()) {
            return std::nullopt;
        }
        message.type = Message::Type::Stop;
        return message;
    }
    if (verb == "CANCEL") {
        if (!rest.empty()) {
            return std::nullopt;
        }
        message.type = Message::Type::Cancel;
        return message;
    }
    if (verb == "TEXT" || verb == "ERROR" || verb == "PARTIAL") {
        auto payload = detail::unescapePayload(rest);
        if (!payload) {
            return std::nullopt;
        }
        if (verb == "PARTIAL") {
            message.type = Message::Type::Partial;
        } else {
            message.type =
                verb == "TEXT" ? Message::Type::Text : Message::Type::Error;
        }
        message.payload = std::move(*payload);
        return message;
    }
    return std::nullopt;
}

class LineBuffer {
public:
    static constexpr size_t kMaxLineBytes = 64 * 1024;

    void append(const char *data, size_t size) {
        if (overflowed_) {
            return;
        }
        pending_.append(data, size);

        size_t start = 0;
        size_t newline = 0;
        while ((newline = pending_.find('\n', start)) != std::string::npos) {
            lines_.emplace_back(pending_.substr(start, newline - start));
            start = newline + 1;
        }
        if (start > 0) {
            pending_.erase(0, start);
        }
        if (pending_.size() > kMaxLineBytes) {
            pending_.clear();
            overflowed_ = true;
        }
    }

    std::optional<std::string> nextLine() {
        if (lines_.empty()) {
            return std::nullopt;
        }
        std::string line = std::move(lines_.front());
        lines_.pop_front();
        return line;
    }

    bool overflowed() const { return overflowed_; }

    void clear() {
        pending_.clear();
        lines_.clear();
        overflowed_ = false;
    }

private:
    std::string pending_;
    std::deque<std::string> lines_;
    bool overflowed_ = false;
};

} // namespace koe

#endif
