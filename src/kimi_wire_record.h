#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace kimi {

enum class Wire_record_kind
{
    OTHER,
    INTERACTION,
    METADATA,
    FORKED,
    USAGE,
    MODEL_REQUEST,
};

// Classify native wire records without retaining their payloads. The complete
// envelope must still finish successfully after type_known() becomes true:
// later duplicate types or malformed syntax cannot hide consumed records.
// Consumed records additionally pass through the unchanged bounded SDK parser.
class Wire_record_envelope
{
public:
    void append(std::string_view bytes);
    bool finish() const;
    bool type_known() const { return m_type_known; }
    Wire_record_kind kind() const { return m_kind; }
    void reset();

private:
    enum class Position : std::uint8_t
    {
        OBJECT_KEY_OR_END,
        OBJECT_KEY,
        OBJECT_COLON,
        OBJECT_VALUE,
        OBJECT_COMMA_OR_END,
        ARRAY_VALUE_OR_END,
        ARRAY_VALUE,
        ARRAY_COMMA_OR_END,
    };
    enum class Token { NONE, STRING, NUMBER, LITERAL };
    enum class String_step { CHARACTER, ESCAPE, HEX, SURROGATE_BACKSLASH, SURROGATE_U };
    enum class String_role { IGNORED, ROOT_KEY, ROOT_TYPE };
    enum class Number_step { MINUS, ZERO, INTEGER, DOT, FRACTION, EXPONENT, EXPONENT_SIGN, EXPONENT_DIGITS };

    void structure(unsigned char byte);
    void begin_value(unsigned char byte);
    void begin_string(bool key, bool discriminator = false);
    void string_byte(unsigned char byte);
    void string_scalar(std::uint32_t scalar);
    void end_string();
    bool number_byte(unsigned char byte);
    void close_container();

    // One grammar state per open container; depth does not create a DOM or
    // recursive call stack. Scalar payload size never grows retained state.
    std::vector<Position> m_stack;
    bool m_started = false;
    bool m_done = false;
    bool m_invalid = false;
    bool m_type_seen = false;
    bool m_type_known = false;
    bool m_root_key_is_type = false;
    Wire_record_kind m_kind = Wire_record_kind::OTHER;

    Token m_token = Token::NONE;
    String_step m_string_step = String_step::CHARACTER;
    String_role m_string_role = String_role::IGNORED;
    bool m_key_string = false;
    // Selected type names are ASCII and at most 20 bytes. Longer/non-ASCII
    // keys and types are still validated, but cannot match those names.
    std::array<char, 20> m_selected_text{};
    std::size_t m_selected_size = 0;
    bool m_selected_other = false;
    std::uint32_t m_scalar = 0;
    std::uint32_t m_scalar_minimum = 0;
    std::uint32_t m_high_surrogate = 0;
    unsigned m_utf8_remaining = 0;
    unsigned m_hex_remaining = 0;

    Number_step m_number_step = Number_step::ZERO;
    std::string_view m_literal;
    std::size_t m_literal_offset = 0;
};

} // namespace kimi
