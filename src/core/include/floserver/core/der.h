#pragma once

// A small DER writer (ITU-T X.690), enough to build X.509 certificates and
// PKCS#8 keys for the local certificate authority (D7). Writing only: the
// certificates FloServer reads back are its own, kept with a small JSON note
// beside them, so no parser is needed.

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace floserver::core::der {

using Bytes = std::vector<std::uint8_t>;

// One element: tag, length (short or long form), content.
[[nodiscard]] Bytes tlv(std::uint8_t tag, std::span<const std::uint8_t> content);

[[nodiscard]] Bytes sequence(std::initializer_list<Bytes> items);
[[nodiscard]] Bytes sequence_of(const std::vector<Bytes>& items);

// A non-negative INTEGER from big-endian bytes: leading zeros dropped, a zero
// byte added when the top bit is set. Empty or all-zero bytes give 0.
[[nodiscard]] Bytes unsigned_integer(std::span<const std::uint8_t> big_endian);
[[nodiscard]] Bytes small_integer(std::uint32_t value);

// "1.2.840.10045.2.1". Throws std::invalid_argument on a malformed OID: they
// are constants in our code, so a bad one is a programming error.
[[nodiscard]] Bytes oid(std::string_view dotted);

[[nodiscard]] Bytes boolean(bool value);
[[nodiscard]] Bytes octet_string(std::span<const std::uint8_t> bytes);
[[nodiscard]] Bytes bit_string(std::span<const std::uint8_t> bytes, std::uint8_t unused_bits = 0);
[[nodiscard]] Bytes utf8_string(std::string_view text);

// UTCTime for 1950-2049, GeneralizedTime otherwise (RFC 5280 4.1.2.5).
[[nodiscard]] Bytes time(std::chrono::sys_seconds t);

// [n] context-specific tag: constructed (0xA0 | n) wraps DER elements,
// primitive (0x80 | n) holds raw content (IMPLICIT tagging of a primitive).
[[nodiscard]] Bytes context_constructed(std::uint8_t n, std::span<const std::uint8_t> content);
[[nodiscard]] Bytes context_primitive(std::uint8_t n, std::span<const std::uint8_t> content);

[[nodiscard]] Bytes concat(std::initializer_list<Bytes> parts);

// "-----BEGIN <label>-----", base64 in 64-character lines, "-----END <label>-----".
[[nodiscard]] std::string pem(std::string_view label, std::span<const std::uint8_t> der);

}  // namespace floserver::core::der
