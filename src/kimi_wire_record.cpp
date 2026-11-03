#include "kimi_wire_record.h"

namespace kimi {
namespace {

bool whitespace(unsigned char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

bool digit(unsigned char byte)
{
    return byte >= '0' && byte <= '9';
}

int hexadecimal(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') { return byte - '0';      }
    if (byte >= 'a' && byte <= 'f') { return byte - 'a' + 10; }
    if (byte >= 'A' && byte <= 'F') { return byte - 'A' + 10; }
    return -1;
}

Wire_record_kind selected_kind(std::string_view type)
{
    if (type == "interaction.request" || type == "interaction.resolved") {
        return Wire_record_kind::INTERACTION;
    }
    if (type == "metadata")     { return Wire_record_kind::METADATA; }
    if (type == "forked")       { return Wire_record_kind::FORKED;   }
    if (type == "usage.record") { return Wire_record_kind::USAGE;    }
    return Wire_record_kind::OTHER;
}

} // namespace

void Wire_record_envelope::append(std::string_view bytes)
{
    for (const unsigned char byte : bytes) {
        if (m_invalid) {
            return;
        }
        if (m_token == Token::STRING) {
            string_byte(byte);
            continue;
        }
        if (m_token == Token::NUMBER) {
            if (number_byte(byte)) {
                continue;
            }
            m_token = Token::NONE;
        }
        if (m_token == Token::LITERAL) {
            if (m_literal_offset < m_literal.size()) {
                m_invalid = byte != (unsigned char)m_literal[m_literal_offset++];
                continue;
            }
            m_token = Token::NONE;
        }
        structure(byte);
    }
}

bool Wire_record_envelope::finish() const
{
    return !m_invalid && m_done && m_token == Token::NONE;
}

void Wire_record_envelope::reset()
{
    *this = Wire_record_envelope{};
}

void Wire_record_envelope::structure(unsigned char byte)
{
    if (whitespace(byte)) {
        return;
    }
    if (!m_started) {
        m_started = true;
        if (byte != '{') {
            m_invalid = true;
            return;
        }
        m_stack.push_back(Position::OBJECT_KEY_OR_END);
        return;
    }
    if (m_done) {
        m_invalid = true;
        return;
    }
    switch (m_stack.back()) {
        case Position::OBJECT_KEY_OR_END:
            if (byte == '}') {
                close_container();
                return;
            }
            [[fallthrough]];
        case Position::OBJECT_KEY:
            if (byte == '"') {
                begin_string(true);
            }
            else {
                m_invalid = true;
            }
            return;
        case Position::OBJECT_COLON:
            if (byte == ':') {
                m_stack.back() = Position::OBJECT_VALUE;
            }
            else {
                m_invalid = true;
            }
            return;
        case Position::OBJECT_VALUE:
        case Position::ARRAY_VALUE:
            begin_value(byte);
            return;
        case Position::OBJECT_COMMA_OR_END:
            if (byte == '}') {
                close_container();
            }
            else
            if (byte == ',') {
                m_stack.back() = Position::OBJECT_KEY;
            }
            else {
                m_invalid = true;
            }
            return;
        case Position::ARRAY_VALUE_OR_END:
            if (byte == ']') {
                close_container();
            }
            else {
                begin_value(byte);
            }
            return;
        case Position::ARRAY_COMMA_OR_END:
            if (byte == ']') {
                close_container();
            }
            else
            if (byte == ',') {
                m_stack.back() = Position::ARRAY_VALUE;
            }
            else {
                m_invalid = true;
            }
            return;
        default:
            m_invalid = true;
            return;
    }
}

void Wire_record_envelope::begin_value(unsigned char byte)
{
    const bool discriminator = m_stack.size() == 1 && m_root_key_is_type;
    if (discriminator && byte != '"') {
        m_type_known = true;
    }
    m_stack.back() = m_stack.back() == Position::OBJECT_VALUE
        ? Position::OBJECT_COMMA_OR_END : Position::ARRAY_COMMA_OR_END;
    if (m_stack.size() == 1) {
        m_root_key_is_type = false;
    }
    switch (byte) {
        case '{':
            m_stack.push_back(Position::OBJECT_KEY_OR_END);
            return;
        case '[':
            m_stack.push_back(Position::ARRAY_VALUE_OR_END);
            return;
        case '"':
            begin_string(false, discriminator);
            return;
        case 't':
            m_literal = "true";
            break;
        case 'f':
            m_literal = "false";
            break;
        case 'n':
            m_literal = "null";
            break;
        default:
            m_token = Token::NUMBER;
            if (byte == '-') {
                m_number_step = Number_step::MINUS;
            }
            else
            if (byte == '0') {
                m_number_step = Number_step::ZERO;
            }
            else
            if (digit(byte)) {
                m_number_step = Number_step::INTEGER;
            }
            else {
                m_invalid = true;
            }
            return;
    }
    m_token = Token::LITERAL;
    m_literal_offset = 1;
}

void Wire_record_envelope::close_container()
{
    m_stack.pop_back();
    m_done = m_stack.empty();
}

void Wire_record_envelope::begin_string(bool key, bool discriminator)
{
    m_token          = Token::STRING;
    m_key_string     = key;
    m_string_step    = String_step::CHARACTER;
    m_string_role    = discriminator ? String_role::ROOT_TYPE :
        (key && m_stack.size() == 1 ? String_role::ROOT_KEY : String_role::IGNORED);
    m_selected_size  = 0;
    m_selected_other = false;
}

void Wire_record_envelope::string_scalar(std::uint32_t scalar)
{
    if (m_string_role == String_role::IGNORED || m_selected_other) {
        return;
    }
    if (scalar > 0x7F || m_selected_size == m_selected_text.size()) {
        m_selected_other = true;
        return;
    }
    m_selected_text[m_selected_size++] = (char)scalar;
}

void Wire_record_envelope::end_string()
{
    const auto selected = m_selected_other ? std::string_view{} :
        std::string_view(m_selected_text.data(), m_selected_size);
    if (m_key_string) {
        if (m_string_role == String_role::ROOT_KEY) {
            m_root_key_is_type = selected == "type";
            if (m_root_key_is_type) {
                m_invalid = m_type_seen;
                m_type_seen = true;
            }
        }
        m_stack.back() = Position::OBJECT_COLON;
    }
    else
    if (m_string_role == String_role::ROOT_TYPE) {
        m_kind = selected_kind(selected);
        m_type_known = true;
    }
    m_token = Token::NONE;
}

void Wire_record_envelope::string_byte(unsigned char byte)
{
    if (m_utf8_remaining) {
        if ((byte & 0xC0) != 0x80) {
            m_invalid = true;
            return;
        }
        m_scalar = (m_scalar << 6) | (byte & 0x3F);
        if (--m_utf8_remaining == 0) {
            if (m_scalar < m_scalar_minimum || m_scalar > 0x10FFFF ||
                (m_scalar >= 0xD800 && m_scalar <= 0xDFFF))
            {
                m_invalid = true;
                return;
            }
            string_scalar(m_scalar);
        }
        return;
    }
    switch (m_string_step) {
        case String_step::CHARACTER:
            if (byte == '"') {
                end_string();
            }
            else
            if (byte == '\\') {
                m_string_step = String_step::ESCAPE;
            }
            else
            if (byte < 0x20) {
                m_invalid = true;
            }
            else
            if (byte < 0x80) {
                string_scalar(byte);
            }
            else
            if (byte >= 0xC2 && byte <= 0xDF) {
                m_scalar         = byte & 0x1F;
                m_scalar_minimum = 0x80;
                m_utf8_remaining = 1;
            }
            else
            if (byte >= 0xE0 && byte <= 0xEF) {
                m_scalar         = byte & 0x0F;
                m_scalar_minimum = 0x800;
                m_utf8_remaining = 2;
            }
            else
            if (byte >= 0xF0 && byte <= 0xF4) {
                m_scalar         = byte & 0x07;
                m_scalar_minimum = 0x10000;
                m_utf8_remaining = 3;
            }
            else {
                m_invalid = true;
            }
            return;
        case String_step::ESCAPE:
            m_string_step = String_step::CHARACTER;
            switch (byte) {
                case '"':
                case '\\':
                case '/':  string_scalar(byte);   return;
                case 'b':  string_scalar('\b');   return;
                case 'f':  string_scalar('\f');   return;
                case 'n':  string_scalar('\n');   return;
                case 'r':  string_scalar('\r');   return;
                case 't':  string_scalar('\t');   return;
                case 'u':
                    m_scalar = 0;
                    m_hex_remaining = 4;
                    m_string_step = String_step::HEX;
                    return;
                default:
                    m_invalid = true;
                    return;
            }
        case String_step::HEX: {
            const auto value = hexadecimal(byte);
            if (value < 0) {
                m_invalid = true;
                return;
            }
            m_scalar = (m_scalar << 4) | (unsigned)value;
            if (--m_hex_remaining) {
                return;
            }
            m_string_step = String_step::CHARACTER;
            if (m_high_surrogate) {
                if (m_scalar < 0xDC00 || m_scalar > 0xDFFF) {
                    m_invalid = true;
                    return;
                }
                string_scalar(0x10000 + ((m_high_surrogate - 0xD800) << 10) + (m_scalar - 0xDC00));
                m_high_surrogate = 0;
            }
            else
            if (m_scalar >= 0xD800 && m_scalar <= 0xDBFF) {
                m_high_surrogate = m_scalar;
                m_string_step = String_step::SURROGATE_BACKSLASH;
            }
            else
            if (m_scalar >= 0xDC00 && m_scalar <= 0xDFFF) {
                m_invalid = true;
            }
            else {
                string_scalar(m_scalar);
            }
            return;
        }
        case String_step::SURROGATE_BACKSLASH:
            m_invalid = byte != '\\';
            m_string_step = String_step::SURROGATE_U;
            return;
        case String_step::SURROGATE_U:
            m_invalid = byte != 'u';
            m_scalar = 0;
            m_hex_remaining = 4;
            m_string_step = String_step::HEX;
            return;
        default:
            m_invalid = true;
            return;
    }
}

bool Wire_record_envelope::number_byte(unsigned char byte)
{
    switch (m_number_step) {
        case Number_step::MINUS:
            if (byte == '0') {
                m_number_step = Number_step::ZERO;
            }
            else
            if (digit(byte)) {
                m_number_step = Number_step::INTEGER;
            }
            else {
                m_invalid = true;
            }
            return true;
        case Number_step::ZERO:
        case Number_step::INTEGER:
            if (byte == '.') {
                m_number_step = Number_step::DOT;
                return true;
            }
            if (byte == 'e' || byte == 'E') {
                m_number_step = Number_step::EXPONENT;
                return true;
            }
            if (digit(byte)) {
                m_invalid = m_number_step == Number_step::ZERO;
                return true;
            }
            return false;
        case Number_step::DOT:
        case Number_step::EXPONENT_SIGN:
            m_invalid = !digit(byte);
            m_number_step = m_number_step == Number_step::DOT
                ? Number_step::FRACTION : Number_step::EXPONENT_DIGITS;
            return true;
        case Number_step::FRACTION:
            if (byte == 'e' || byte == 'E') {
                m_number_step = Number_step::EXPONENT;
                return true;
            }
            return digit(byte);
        case Number_step::EXPONENT:
            if (byte == '+' || byte == '-') {
                m_number_step = Number_step::EXPONENT_SIGN;
            }
            else {
                m_invalid = !digit(byte);
                m_number_step = Number_step::EXPONENT_DIGITS;
            }
            return true;
        case Number_step::EXPONENT_DIGITS:
            return digit(byte);
        default:
            m_invalid = true;
            return true;
    }
}

} // namespace kimi
