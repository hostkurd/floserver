#include "floserver/core/der.h"

#include "floserver/core/encoding.h"

#include <array>
#include <cstdio>
#include <stdexcept>

namespace floserver::core::der {

namespace {

void append(Bytes& out, std::span<const std::uint8_t> more) { out.insert(out.end(), more.begin(), more.end()); }

void append_base128(Bytes& out, std::uint64_t value) {
    std::array<std::uint8_t, 10> tmp{};
    std::size_t n = 0;
    do {
        tmp[n++] = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
    } while (value != 0);
    while (n > 0) {
        --n;
        out.push_back(static_cast<std::uint8_t>(tmp[n] | (n > 0 ? 0x80 : 0x00)));
    }
}

}  // namespace

Bytes tlv(std::uint8_t tag, std::span<const std::uint8_t> content) {
    Bytes out;
    out.reserve(content.size() + 6);
    out.push_back(tag);
    const std::size_t len = content.size();
    if (len < 0x80) {
        out.push_back(static_cast<std::uint8_t>(len));
    } else {
        Bytes len_bytes;
        for (std::size_t v = len; v != 0; v >>= 8) {
            len_bytes.insert(len_bytes.begin(), static_cast<std::uint8_t>(v & 0xFF));
        }
        out.push_back(static_cast<std::uint8_t>(0x80 | len_bytes.size()));
        append(out, len_bytes);
    }
    append(out, content);
    return out;
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) {
        append(out, p);
    }
    return out;
}

Bytes sequence(std::initializer_list<Bytes> items) { return tlv(0x30, concat(items)); }

Bytes sequence_of(const std::vector<Bytes>& items) {
    Bytes content;
    for (const Bytes& i : items) {
        append(content, i);
    }
    return tlv(0x30, content);
}

Bytes unsigned_integer(std::span<const std::uint8_t> big_endian) {
    std::size_t start = 0;
    while (start < big_endian.size() && big_endian[start] == 0) {
        ++start;
    }
    Bytes content;
    if (start == big_endian.size()) {
        content.push_back(0);
    } else {
        if ((big_endian[start] & 0x80) != 0) {
            content.push_back(0);
        }
        content.insert(content.end(), big_endian.begin() + static_cast<std::ptrdiff_t>(start), big_endian.end());
    }
    return tlv(0x02, content);
}

Bytes small_integer(std::uint32_t value) {
    const Bytes be = {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
                      static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
    return unsigned_integer(be);
}

Bytes oid(std::string_view dotted) {
    std::vector<std::uint64_t> arcs;
    std::uint64_t current = 0;
    bool have_digit = false;
    for (const char c : dotted) {
        if (c >= '0' && c <= '9') {
            current = current * 10 + static_cast<std::uint64_t>(c - '0');
            have_digit = true;
            if (current > (std::uint64_t{1} << 40)) {
                throw std::invalid_argument("OID arc too large");
            }
        } else if (c == '.' && have_digit) {
            arcs.push_back(current);
            current = 0;
            have_digit = false;
        } else {
            throw std::invalid_argument("malformed OID");
        }
    }
    if (!have_digit) {
        throw std::invalid_argument("malformed OID");
    }
    arcs.push_back(current);
    if (arcs.size() < 2 || arcs[0] > 2 || (arcs[0] < 2 && arcs[1] > 39)) {
        throw std::invalid_argument("malformed OID");
    }
    Bytes content;
    append_base128(content, arcs[0] * 40 + arcs[1]);
    for (std::size_t i = 2; i < arcs.size(); ++i) {
        append_base128(content, arcs[i]);
    }
    return tlv(0x06, content);
}

Bytes boolean(bool value) {
    const Bytes content = {static_cast<std::uint8_t>(value ? 0xFF : 0x00)};
    return tlv(0x01, content);
}

Bytes octet_string(std::span<const std::uint8_t> bytes) { return tlv(0x04, bytes); }

Bytes bit_string(std::span<const std::uint8_t> bytes, std::uint8_t unused_bits) {
    Bytes content;
    content.reserve(bytes.size() + 1);
    content.push_back(unused_bits);
    append(content, bytes);
    return tlv(0x03, content);
}

Bytes utf8_string(std::string_view text) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(text.data());
    return tlv(0x0C, std::span<const std::uint8_t>(p, text.size()));
}

Bytes time(std::chrono::sys_seconds t) {
    const auto day = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd(day);
    const std::chrono::hh_mm_ss hms(t - day);
    const int year = static_cast<int>(ymd.year());
    const bool utc = year >= 1950 && year < 2050;
    std::array<char, 32> buf{};
    const int n = std::snprintf(buf.data(), buf.size(), utc ? "%02d%02u%02u%02d%02d%02dZ" : "%04d%02u%02u%02d%02d%02dZ",
                                utc ? year % 100 : year, static_cast<unsigned>(ymd.month()),
                                static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                                static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count()));
    const auto* p = reinterpret_cast<const std::uint8_t*>(buf.data());
    return tlv(utc ? 0x17 : 0x18, std::span<const std::uint8_t>(p, static_cast<std::size_t>(n)));
}

Bytes context_constructed(std::uint8_t n, std::span<const std::uint8_t> content) {
    return tlv(static_cast<std::uint8_t>(0xA0 | n), content);
}

Bytes context_primitive(std::uint8_t n, std::span<const std::uint8_t> content) {
    return tlv(static_cast<std::uint8_t>(0x80 | n), content);
}

std::string pem(std::string_view label, std::span<const std::uint8_t> der) {
    const std::string b64 = to_base64(der);
    std::string out = "-----BEGIN " + std::string(label) + "-----\n";
    for (std::size_t i = 0; i < b64.size(); i += 64) {
        out += b64.substr(i, 64);
        out += '\n';
    }
    out += "-----END " + std::string(label) + "-----\n";
    return out;
}

}  // namespace floserver::core::der
