#include "sorx/config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sorx::config {
namespace {

enum class SectionKind : std::uint8_t {
    Sorx,
    Instrument,
    Venue,
    Route,
    Unknown,
};

struct RawField final {
    std::string value;
    std::size_t line{};
};

struct RawSection final {
    SectionKind kind{SectionKind::Unknown};
    std::size_t line{};
    std::string label;
    std::uint32_t first_id{};
    std::uint32_t second_id{};
    std::map<std::string, RawField> fields;
};

struct RawDocument final {
    std::vector<RawSection> sections;
    std::vector<ConfigurationError> errors;
};

[[nodiscard]] std::string_view trim(const std::string_view value) noexcept {
    std::size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

[[nodiscard]] std::vector<std::string_view> split_words(const std::string_view value) {
    std::vector<std::string_view> words;
    std::size_t position = 0;
    while (position < value.size()) {
        while (position < value.size() &&
               std::isspace(static_cast<unsigned char>(value[position])) != 0) {
            ++position;
        }
        const auto first = position;
        while (position < value.size() &&
               std::isspace(static_cast<unsigned char>(value[position])) == 0) {
            ++position;
        }
        if (first != position) {
            words.push_back(value.substr(first, position - first));
        }
    }
    return words;
}

void add_error(
    std::vector<ConfigurationError>& errors,
    const ConfigurationErrorCode code,
    const std::size_t line,
    std::string section,
    std::string field,
    std::string message) {
    errors.push_back(ConfigurationError{
        code,
        line,
        std::move(section),
        std::move(field),
        std::move(message),
    });
}

struct UnsignedParse final {
    bool ok{};
    bool overflow{};
    std::uint64_t value{};
};

[[nodiscard]] UnsignedParse parse_unsigned_text(const std::string_view text) noexcept {
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](const unsigned char value) {
            return std::isdigit(value) != 0;
        })) {
        return {};
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec == std::errc::result_out_of_range) {
        return UnsignedParse{false, true, 0};
    }
    return UnsignedParse{
        result.ec == std::errc{} && result.ptr == text.data() + text.size(),
        false,
        value,
    };
}

struct SignedParse final {
    bool ok{};
    bool overflow{};
    std::int64_t value{};
};

[[nodiscard]] SignedParse parse_signed_text(const std::string_view text) noexcept {
    if (text.empty()) {
        return {};
    }
    const auto digits = text.front() == '-' ? text.substr(1) : text;
    if (digits.empty() ||
        !std::all_of(digits.begin(), digits.end(), [](const unsigned char value) {
            return std::isdigit(value) != 0;
        })) {
        return {};
    }
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec == std::errc::result_out_of_range) {
        return SignedParse{false, true, 0};
    }
    return SignedParse{
        result.ec == std::errc{} && result.ptr == text.data() + text.size(),
        false,
        value,
    };
}

[[nodiscard]] bool valid_key(const std::string_view key) noexcept {
    if (key.empty() || key.front() < 'a' || key.front() > 'z') {
        return false;
    }
    return std::all_of(key.begin(), key.end(), [](const char value) {
        return (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '_';
    });
}

[[nodiscard]] std::optional<std::uint32_t> parse_section_id(
    const std::string_view token,
    const std::size_t line,
    const std::string& section,
    std::vector<ConfigurationError>& errors) {
    const auto parsed = parse_unsigned_text(token);
    if (!parsed.ok) {
        add_error(
            errors,
            parsed.overflow ? ConfigurationErrorCode::Overflow
                            : ConfigurationErrorCode::InvalidValue,
            line,
            section,
            {},
            parsed.overflow ? "section ID is out of range"
                            : "section ID must be a positive decimal integer");
        return std::nullopt;
    }
    if (parsed.value == 0 ||
        parsed.value > std::numeric_limits<std::uint32_t>::max()) {
        add_error(
            errors,
            parsed.value > std::numeric_limits<std::uint32_t>::max()
                ? ConfigurationErrorCode::Overflow
                : ConfigurationErrorCode::InvalidValue,
            line,
            section,
            {},
            "section ID must fit a nonzero uint32 value");
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(parsed.value);
}

[[nodiscard]] RawDocument parse_raw_document(const std::string_view text) {
    RawDocument document;
    std::optional<std::size_t> current_section;
    std::set<std::tuple<SectionKind, std::uint32_t, std::uint32_t>> section_keys;

    std::size_t position = 0;
    std::size_t line_number = 1;
    bool done = false;
    while (!done) {
        const auto newline = text.find('\n', position);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        auto line = text.substr(position, end - position);
        done = newline == std::string_view::npos;
        position = done ? text.size() : newline + 1;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line_number == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xefU &&
            static_cast<unsigned char>(line[1]) == 0xbbU &&
            static_cast<unsigned char>(line[2]) == 0xbfU) {
            line.remove_prefix(3);
        }

        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#' || stripped.front() == ';') {
            ++line_number;
            continue;
        }

        if (stripped.front() == '[') {
            current_section.reset();
            if (stripped.size() < 2 || stripped.back() != ']') {
                add_error(
                    document.errors,
                    ConfigurationErrorCode::Syntax,
                    line_number,
                    {},
                    {},
                    "section header must end with ']'");
                ++line_number;
                continue;
            }

            const auto body = trim(stripped.substr(1, stripped.size() - 2));
            const auto words = split_words(body);
            RawSection section;
            section.line = line_number;
            section.label = std::string{body};

            bool valid_section = true;
            if (words.size() == 1 && words[0] == "sorx") {
                section.kind = SectionKind::Sorx;
            } else if (words.size() == 2 && words[0] == "instrument") {
                section.kind = SectionKind::Instrument;
                const auto id = parse_section_id(
                    words[1], line_number, section.label, document.errors);
                if (id) {
                    section.first_id = *id;
                } else {
                    valid_section = false;
                }
            } else if (words.size() == 2 && words[0] == "venue") {
                section.kind = SectionKind::Venue;
                const auto id = parse_section_id(
                    words[1], line_number, section.label, document.errors);
                if (id) {
                    section.first_id = *id;
                } else {
                    valid_section = false;
                }
            } else if (words.size() == 3 && words[0] == "route") {
                section.kind = SectionKind::Route;
                const auto instrument = parse_section_id(
                    words[1], line_number, section.label, document.errors);
                const auto venue = parse_section_id(
                    words[2], line_number, section.label, document.errors);
                if (instrument && venue) {
                    section.first_id = *instrument;
                    section.second_id = *venue;
                } else {
                    valid_section = false;
                }
            } else {
                section.kind = SectionKind::Unknown;
                valid_section = false;
                add_error(
                    document.errors,
                    ConfigurationErrorCode::UnknownSection,
                    line_number,
                    section.label,
                    {},
                    "unknown section or invalid section arity");
            }

            document.sections.push_back(std::move(section));
            current_section = document.sections.size() - 1;
            if (valid_section) {
                const auto& stored = document.sections.back();
                const auto key = std::tuple{
                    stored.kind,
                    stored.first_id,
                    stored.second_id,
                };
                if (!section_keys.insert(key).second) {
                    add_error(
                        document.errors,
                        ConfigurationErrorCode::DuplicateSection,
                        line_number,
                        stored.label,
                        {},
                        "section identity is duplicated");
                }
            }
            ++line_number;
            continue;
        }

        if (!current_section) {
            add_error(
                document.errors,
                ConfigurationErrorCode::Syntax,
                line_number,
                {},
                {},
                "field appears before a section header");
            ++line_number;
            continue;
        }

        const auto equals = stripped.find('=');
        if (equals == std::string_view::npos) {
            add_error(
                document.errors,
                ConfigurationErrorCode::Syntax,
                line_number,
                document.sections[*current_section].label,
                {},
                "field must use key=value syntax");
            ++line_number;
            continue;
        }
        const auto key_view = trim(stripped.substr(0, equals));
        const auto value_view = trim(stripped.substr(equals + 1));
        if (!valid_key(key_view)) {
            add_error(
                document.errors,
                ConfigurationErrorCode::Syntax,
                line_number,
                document.sections[*current_section].label,
                std::string{key_view},
                "field name must use lowercase letters, digits, and underscores");
            ++line_number;
            continue;
        }

        auto& section = document.sections[*current_section];
        const std::string key{key_view};
        if (!section.fields.emplace(
                key,
                RawField{std::string{value_view}, line_number}).second) {
            add_error(
                document.errors,
                ConfigurationErrorCode::DuplicateField,
                line_number,
                section.label,
                key,
                "field is duplicated in its section");
        }
        ++line_number;
    }
    return document;
}

[[nodiscard]] bool contains_name(
    const std::initializer_list<std::string_view> names,
    const std::string_view value) noexcept {
    return std::find(names.begin(), names.end(), value) != names.end();
}

void validate_fields(
    const RawSection& section,
    const std::initializer_list<std::string_view> required,
    const std::initializer_list<std::string_view> optional,
    std::vector<ConfigurationError>& errors) {
    for (const auto& [key, field] : section.fields) {
        if (!contains_name(required, key) && !contains_name(optional, key)) {
            add_error(
                errors,
                ConfigurationErrorCode::UnknownField,
                field.line,
                section.label,
                key,
                "field is not valid for this section");
        }
    }
    for (const auto name : required) {
        if (!section.fields.contains(std::string{name})) {
            add_error(
                errors,
                ConfigurationErrorCode::MissingField,
                section.line,
                section.label,
                std::string{name},
                "required field is missing");
        }
    }
}

[[nodiscard]] const RawField* find_field(
    const RawSection& section,
    const std::string_view name) noexcept {
    const auto found = section.fields.find(std::string{name});
    return found == section.fields.end() ? nullptr : &found->second;
}

[[nodiscard]] std::optional<std::string> parse_identity_field(
    const RawSection& section,
    const std::string_view name,
    std::vector<ConfigurationError>& errors) {
    const auto* field = find_field(section, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    const auto& value = field->value;
    const bool has_control = std::any_of(value.begin(), value.end(), [](const char character) {
        return static_cast<unsigned char>(character) < 0x20U ||
               static_cast<unsigned char>(character) == 0x7fU;
    });
    if (value.empty() || value.size() > 255 || has_control) {
        add_error(
            errors,
            ConfigurationErrorCode::InvalidValue,
            field->line,
            section.label,
            std::string{name},
            "identity must contain 1 to 255 non-control bytes");
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned_field(
    const RawSection& section,
    const std::string_view name,
    const std::uint64_t minimum,
    const std::uint64_t maximum,
    std::vector<ConfigurationError>& errors) {
    const auto* field = find_field(section, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    const auto parsed = parse_unsigned_text(field->value);
    if (!parsed.ok) {
        add_error(
            errors,
            parsed.overflow ? ConfigurationErrorCode::Overflow
                            : ConfigurationErrorCode::InvalidValue,
            field->line,
            section.label,
            std::string{name},
            parsed.overflow ? "unsigned integer is out of range"
                            : "value must be an unsigned decimal integer");
        return std::nullopt;
    }
    if (parsed.value < minimum || parsed.value > maximum) {
        add_error(
            errors,
            parsed.value > maximum ? ConfigurationErrorCode::Overflow
                                   : ConfigurationErrorCode::InvalidValue,
            field->line,
            section.label,
            std::string{name},
            "unsigned integer violates the configured range");
        return std::nullopt;
    }
    return parsed.value;
}

[[nodiscard]] std::optional<std::int64_t> parse_signed_field(
    const RawSection& section,
    const std::string_view name,
    const std::int64_t minimum,
    const std::int64_t maximum,
    std::vector<ConfigurationError>& errors) {
    const auto* field = find_field(section, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    const auto parsed = parse_signed_text(field->value);
    if (!parsed.ok) {
        add_error(
            errors,
            parsed.overflow ? ConfigurationErrorCode::Overflow
                            : ConfigurationErrorCode::InvalidValue,
            field->line,
            section.label,
            std::string{name},
            parsed.overflow ? "signed integer is out of range"
                            : "value must be a signed decimal integer");
        return std::nullopt;
    }
    if (parsed.value < minimum || parsed.value > maximum) {
        add_error(
            errors,
            ConfigurationErrorCode::InvalidValue,
            field->line,
            section.label,
            std::string{name},
            "signed integer violates the configured range");
        return std::nullopt;
    }
    return parsed.value;
}

[[nodiscard]] std::optional<bool> parse_bool_field(
    const RawSection& section,
    const std::string_view name,
    std::vector<ConfigurationError>& errors) {
    const auto* field = find_field(section, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (field->value == "true") {
        return true;
    }
    if (field->value == "false") {
        return false;
    }
    add_error(
        errors,
        ConfigurationErrorCode::InvalidValue,
        field->line,
        section.label,
        std::string{name},
        "boolean must be exactly 'true' or 'false'");
    return std::nullopt;
}

[[nodiscard]] std::optional<AdminState> parse_admin_field(
    const RawSection& section,
    std::vector<ConfigurationError>& errors) {
    constexpr std::string_view name{"admin_state"};
    const auto* field = find_field(section, name);
    if (field == nullptr) {
        return std::nullopt;
    }
    if (field->value == "UP") {
        return AdminState::Up;
    }
    if (field->value == "DOWN") {
        return AdminState::Down;
    }
    add_error(
        errors,
        ConfigurationErrorCode::InvalidValue,
        field->line,
        section.label,
        std::string{name},
        "admin_state must be exactly 'UP' or 'DOWN'");
    return std::nullopt;
}

[[nodiscard]] std::uint64_t size_t_max_as_u64() noexcept {
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
        return std::numeric_limits<std::uint64_t>::max();
    } else {
        return static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    }
}

void add_semantic_error(
    std::vector<ConfigurationError>& errors,
    const ConfigurationErrorCode code,
    const RawSection& section,
    const std::string_view field,
    std::string message) {
    const auto* source = find_field(section, field);
    add_error(
        errors,
        code,
        source == nullptr ? section.line : source->line,
        section.label,
        std::string{field},
        std::move(message));
}

struct CanonicalWriter final {
    std::vector<std::uint8_t> bytes;

    void put_u8(const std::uint8_t value) {
        bytes.push_back(value);
    }

    void put_u32(const std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<std::uint8_t>(
                (value >> static_cast<unsigned int>(shift)) & 0xffU));
        }
    }

    void put_u64(const std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            bytes.push_back(static_cast<std::uint8_t>(
                (value >> static_cast<unsigned int>(shift)) & 0xffU));
        }
    }

    void put_i64(const std::int64_t value) {
        put_u64(static_cast<std::uint64_t>(value));
    }

    void put_bool(const bool value) {
        put_u8(value ? 1U : 0U);
    }

    void put_string(const std::string_view value) {
        put_u64(static_cast<std::uint64_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};

[[nodiscard]] constexpr std::uint32_t rotate_right(
    const std::uint32_t value,
    const unsigned int count) noexcept {
    return (value >> count) | (value << (32U - count));
}

class Sha256 final {
public:
    void update(const std::span<const std::uint8_t> input) {
        for (const auto byte : input) {
            buffer_[buffer_size_++] = byte;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_);
                bit_length_ += 512U;
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] Sha256Digest finish() {
        const auto total_bits =
            bit_length_ + static_cast<std::uint64_t>(buffer_size_) * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56U) {
            while (buffer_size_ < buffer_.size()) {
                buffer_[buffer_size_++] = 0;
            }
            transform(buffer_);
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56U) {
            buffer_[buffer_size_++] = 0;
        }
        for (unsigned int index = 0; index < 8U; ++index) {
            buffer_[63U - index] = static_cast<std::uint8_t>(
                total_bits >> (index * 8U));
        }
        transform(buffer_);

        Sha256Digest digest{};
        for (std::size_t word = 0; word < state_.size(); ++word) {
            digest[word * 4U] = static_cast<std::uint8_t>(state_[word] >> 24U);
            digest[word * 4U + 1U] =
                static_cast<std::uint8_t>(state_[word] >> 16U);
            digest[word * 4U + 2U] =
                static_cast<std::uint8_t>(state_[word] >> 8U);
            digest[word * 4U + 3U] =
                static_cast<std::uint8_t>(state_[word]);
        }
        return digest;
    }

private:
    void transform(const std::array<std::uint8_t, 64>& block) {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };

        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto offset = index * 4U;
            schedule[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24U) |
                (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                static_cast<std::uint32_t>(block[offset + 3U]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            const auto before_two = schedule[index - 2U];
            const auto before_fifteen = schedule[index - 15U];
            const auto small_one =
                rotate_right(before_two, 17U) ^
                rotate_right(before_two, 19U) ^ (before_two >> 10U);
            const auto small_zero =
                rotate_right(before_fifteen, 7U) ^
                rotate_right(before_fifteen, 18U) ^ (before_fifteen >> 3U);
            schedule[index] =
                schedule[index - 16U] + small_zero +
                schedule[index - 7U] + small_one;
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const auto big_one =
                rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temporary_one =
                h + big_one + choose + constants[index] + schedule[index];
            const auto big_zero =
                rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary_two = big_zero + majority;

            h = g;
            g = f;
            f = e;
            e = d + temporary_one;
            d = c;
            c = b;
            b = a;
            a = temporary_one + temporary_two;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{};
    std::uint64_t bit_length_{};
};

}  // namespace

ConfigurationResult ConfigurationLoader::parse_ini(const std::string_view text) {
    auto raw = parse_raw_document(text);
    ConfigurationResult result;
    result.errors = std::move(raw.errors);

    const std::initializer_list<std::string_view> sorx_fields{
        "schema_version",
        "account_id",
        "maximum_aggressive_venues",
        "ingress_capacity",
        "risk_generation",
    };
    const std::initializer_list<std::string_view> instrument_fields{
        "symbol",
        "allowed",
        "maximum_parent_quantity",
        "maximum_child_quantity",
        "maximum_parent_notional",
        "maximum_child_notional",
        "minimum_price",
        "maximum_price",
        "maximum_open_parents",
        "maximum_open_children",
        "maximum_gross_outstanding",
    };
    const std::initializer_list<std::string_view> venue_fields{
        "name",
        "admin_state",
        "risk_allowed",
        "maximum_outstanding",
        "market_data_ingress_capacity",
        "order_entry_ingress_capacity",
        "cancel_egress_capacity",
        "normal_egress_capacity",
        "market_data_channel_id",
        "market_data_profile_id",
        "order_entry_profile_id",
        "market_data_profile",
        "market_data_session",
        "order_entry_profile",
        "order_entry_session",
        "dictionary_identity",
        "sender_comp_id",
        "target_comp_id",
    };
    const std::initializer_list<std::string_view> venue_optional{
        "credential_reference",
    };
    const std::initializer_list<std::string_view> route_fields{
        "wire_symbol",
        "price_numerator",
        "price_denominator",
        "quantity_numerator",
        "quantity_denominator",
        "aggressive_ioc",
        "passive_gtc",
        "allocation_cap",
        "aggressive_priority",
        "passive_priority",
        "configured_latency_ns",
    };

    std::vector<const RawSection*> sorx_sections;
    std::vector<const RawSection*> instrument_sections;
    std::vector<const RawSection*> venue_sections;
    std::vector<const RawSection*> route_sections;
    for (const auto& section : raw.sections) {
        switch (section.kind) {
            case SectionKind::Sorx:
                sorx_sections.push_back(&section);
                validate_fields(section, sorx_fields, {}, result.errors);
                break;
            case SectionKind::Instrument:
                instrument_sections.push_back(&section);
                validate_fields(section, instrument_fields, {}, result.errors);
                break;
            case SectionKind::Venue:
                venue_sections.push_back(&section);
                validate_fields(section, venue_fields, venue_optional, result.errors);
                break;
            case SectionKind::Route:
                route_sections.push_back(&section);
                validate_fields(section, route_fields, {}, result.errors);
                break;
            case SectionKind::Unknown:
                break;
        }
    }

    if (sorx_sections.size() != 1U) {
        add_error(
            result.errors,
            sorx_sections.empty() ? ConfigurationErrorCode::MissingField
                                  : ConfigurationErrorCode::DuplicateSection,
            sorx_sections.empty() ? 0U : sorx_sections[1]->line,
            "sorx",
            {},
            "configuration must contain exactly one [sorx] section");
    }
    if (instrument_sections.empty()) {
        add_error(
            result.errors,
            ConfigurationErrorCode::MissingField,
            0,
            "document",
            "instrument",
            "at least one instrument section is required");
    }
    if (venue_sections.empty()) {
        add_error(
            result.errors,
            ConfigurationErrorCode::MissingField,
            0,
            "document",
            "venue",
            "at least one venue section is required");
    }
    if (route_sections.empty()) {
        add_error(
            result.errors,
            ConfigurationErrorCode::MissingField,
            0,
            "document",
            "route",
            "at least one route section is required");
    }

    std::optional<std::uint32_t> schema_version;
    std::optional<std::string> account_id;
    std::optional<std::size_t> maximum_aggressive_venues;
    std::optional<std::size_t> ingress_capacity;
    std::optional<std::uint64_t> risk_generation;
    if (!sorx_sections.empty()) {
        const auto& section = *sorx_sections.front();
        const auto schema = parse_unsigned_field(
            section,
            "schema_version",
            1,
            std::numeric_limits<std::uint32_t>::max(),
            result.errors);
        account_id = parse_identity_field(section, "account_id", result.errors);
        const auto maximum = parse_unsigned_field(
            section,
            "maximum_aggressive_venues",
            1,
            size_t_max_as_u64(),
            result.errors);
        const auto ingress = parse_unsigned_field(
            section,
            "ingress_capacity",
            1,
            size_t_max_as_u64(),
            result.errors);
        risk_generation = parse_unsigned_field(
            section,
            "risk_generation",
            1,
            std::numeric_limits<std::uint64_t>::max(),
            result.errors);
        if (schema) {
            schema_version = static_cast<std::uint32_t>(*schema);
            if (*schema_version != 1U) {
                add_semantic_error(
                    result.errors,
                    ConfigurationErrorCode::InvalidValue,
                    section,
                    "schema_version",
                    "only schema_version=1 is supported");
            }
        }
        if (maximum) {
            maximum_aggressive_venues = static_cast<std::size_t>(*maximum);
        }
        if (ingress) {
            ingress_capacity = static_cast<std::size_t>(*ingress);
        }
    }

    std::vector<InstrumentConfiguration> instruments;
    std::map<InstrumentId, std::size_t> instrument_indexes;
    std::map<InstrumentId, const RawSection*> instrument_sources;
    std::map<std::string, InstrumentId> instrument_symbols;
    for (const auto* section : instrument_sections) {
        const InstrumentId id{section->first_id};
        const auto symbol = parse_identity_field(*section, "symbol", result.errors);
        const auto allowed = parse_bool_field(*section, "allowed", result.errors);
        const auto maximum_parent_quantity = parse_unsigned_field(
            *section, "maximum_parent_quantity", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto maximum_child_quantity = parse_unsigned_field(
            *section, "maximum_child_quantity", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto maximum_parent_notional = parse_unsigned_field(
            *section, "maximum_parent_notional", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto maximum_child_notional = parse_unsigned_field(
            *section, "maximum_child_notional", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto minimum_price = parse_signed_field(
            *section, "minimum_price", 1,
            std::numeric_limits<std::int64_t>::max(), result.errors);
        const auto maximum_price = parse_signed_field(
            *section, "maximum_price", 1,
            std::numeric_limits<std::int64_t>::max(), result.errors);
        const auto maximum_open_parents = parse_unsigned_field(
            *section, "maximum_open_parents", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto maximum_open_children = parse_unsigned_field(
            *section, "maximum_open_children", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto maximum_gross_outstanding = parse_unsigned_field(
            *section, "maximum_gross_outstanding", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);

        if (!symbol || !allowed.has_value() || !maximum_parent_quantity ||
            !maximum_child_quantity || !maximum_parent_notional ||
            !maximum_child_notional || !minimum_price || !maximum_price ||
            !maximum_open_parents || !maximum_open_children ||
            !maximum_gross_outstanding) {
            continue;
        }

        if (*maximum_child_quantity > *maximum_parent_quantity) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::InvalidValue,
                *section,
                "maximum_child_quantity",
                "maximum child quantity cannot exceed maximum parent quantity");
        }
        if (*maximum_child_notional > *maximum_parent_notional) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::InvalidValue,
                *section,
                "maximum_child_notional",
                "maximum child notional cannot exceed maximum parent notional");
        }
        if (*minimum_price > *maximum_price) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::InvalidValue,
                *section,
                "maximum_price",
                "maximum price must not be below minimum price");
        }
        if (*maximum_gross_outstanding < *maximum_parent_quantity) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::InvalidValue,
                *section,
                "maximum_gross_outstanding",
                "gross outstanding limit must cover one maximum parent");
        }
        if (!checked_notional(
                Price{*maximum_price}, Quantity{*maximum_parent_quantity})) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::Overflow,
                *section,
                "maximum_parent_quantity",
                "maximum price times maximum parent quantity overflows notional");
        }
        if (!checked_notional(
                Price{*maximum_price}, Quantity{*maximum_child_quantity})) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::Overflow,
                *section,
                "maximum_child_quantity",
                "maximum price times maximum child quantity overflows notional");
        }

        if (instrument_indexes.contains(id)) {
            add_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                section->line,
                section->label,
                {},
                "instrument ID is duplicated");
            continue;
        }
        const auto [symbol_it, symbol_inserted] = instrument_symbols.emplace(*symbol, id);
        if (!symbol_inserted && symbol_it->second != id) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                *section,
                "symbol",
                "canonical instrument symbol is duplicated");
        }

        InstrumentRiskLimit risk{
            id,
            *allowed,
            Quantity{*maximum_parent_quantity},
            Quantity{*maximum_child_quantity},
            Notional{*maximum_parent_notional},
            Notional{*maximum_child_notional},
            Price{*minimum_price},
            Price{*maximum_price},
            *maximum_open_parents,
            *maximum_open_children,
            Quantity{*maximum_gross_outstanding},
        };
        instrument_indexes.emplace(id, instruments.size());
        instrument_sources.emplace(id, section);
        instruments.push_back(InstrumentConfiguration{id, *symbol, risk});
    }

    std::vector<VenueConfiguration> venues;
    std::map<VenueId, std::size_t> venue_indexes;
    std::map<VenueId, const RawSection*> venue_sources;
    std::map<std::string, VenueId> venue_names;
    std::map<ChannelId, VenueId> channel_ids;
    std::map<std::string, VenueId> market_data_sessions;
    std::map<std::string, VenueId> order_entry_sessions;
    for (const auto* section : venue_sections) {
        const VenueId id{section->first_id};
        const auto name = parse_identity_field(*section, "name", result.errors);
        const auto admin = parse_admin_field(*section, result.errors);
        const auto risk_allowed = parse_bool_field(*section, "risk_allowed", result.errors);
        const auto maximum_outstanding = parse_unsigned_field(
            *section, "maximum_outstanding", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto md_ingress = parse_unsigned_field(
            *section, "market_data_ingress_capacity", 1,
            size_t_max_as_u64(), result.errors);
        const auto oe_ingress = parse_unsigned_field(
            *section, "order_entry_ingress_capacity", 1,
            size_t_max_as_u64(), result.errors);
        const auto cancel_egress = parse_unsigned_field(
            *section, "cancel_egress_capacity", 1,
            size_t_max_as_u64(), result.errors);
        const auto normal_egress = parse_unsigned_field(
            *section, "normal_egress_capacity", 1,
            size_t_max_as_u64(), result.errors);
        const auto channel = parse_unsigned_field(
            *section, "market_data_channel_id", 1,
            std::numeric_limits<std::uint32_t>::max(), result.errors);
        const auto md_profile_id = parse_unsigned_field(
            *section, "market_data_profile_id", 1,
            std::numeric_limits<std::uint32_t>::max(), result.errors);
        const auto oe_profile_id = parse_unsigned_field(
            *section, "order_entry_profile_id", 1,
            std::numeric_limits<std::uint32_t>::max(), result.errors);
        const auto md_profile =
            parse_identity_field(*section, "market_data_profile", result.errors);
        const auto md_session =
            parse_identity_field(*section, "market_data_session", result.errors);
        const auto oe_profile =
            parse_identity_field(*section, "order_entry_profile", result.errors);
        const auto oe_session =
            parse_identity_field(*section, "order_entry_session", result.errors);
        const auto dictionary =
            parse_identity_field(*section, "dictionary_identity", result.errors);
        const auto sender =
            parse_identity_field(*section, "sender_comp_id", result.errors);
        const auto target =
            parse_identity_field(*section, "target_comp_id", result.errors);
        std::optional<std::string> credential_reference;
        if (find_field(*section, "credential_reference") != nullptr) {
            credential_reference = parse_identity_field(
                *section, "credential_reference", result.errors);
        }

        if (!name || !admin || !risk_allowed.has_value() || !maximum_outstanding ||
            !md_ingress || !oe_ingress || !cancel_egress || !normal_egress ||
            !channel || !md_profile_id || !oe_profile_id || !md_profile ||
            !md_session || !oe_profile || !oe_session || !dictionary ||
            !sender || !target) {
            continue;
        }
        if (venue_indexes.contains(id)) {
            add_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                section->line,
                section->label,
                {},
                "venue ID is duplicated");
            continue;
        }
        const auto [name_it, name_inserted] = venue_names.emplace(*name, id);
        if (!name_inserted && name_it->second != id) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                *section,
                "name",
                "venue name is duplicated");
        }
        const ChannelId channel_id{static_cast<std::uint32_t>(*channel)};
        const auto [channel_it, channel_inserted] = channel_ids.emplace(channel_id, id);
        if (!channel_inserted && channel_it->second != id) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                *section,
                "market_data_channel_id",
                "market-data channel ID is duplicated");
        }
        const auto [md_session_it, md_session_inserted] =
            market_data_sessions.emplace(*md_session, id);
        if (!md_session_inserted && md_session_it->second != id) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                *section,
                "market_data_session",
                "market-data session identity is duplicated");
        }
        const auto [oe_session_it, oe_session_inserted] =
            order_entry_sessions.emplace(*oe_session, id);
        if (!oe_session_inserted && oe_session_it->second != id) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                *section,
                "order_entry_session",
                "order-entry session identity is duplicated");
        }

        VenueRiskLimit risk{id, *risk_allowed, Quantity{*maximum_outstanding}};
        VenueConfiguration venue{
            id,
            *name,
            *admin,
            risk,
            static_cast<std::size_t>(*md_ingress),
            static_cast<std::size_t>(*oe_ingress),
            static_cast<std::size_t>(*cancel_egress),
            static_cast<std::size_t>(*normal_egress),
            channel_id,
            ProfileId{static_cast<std::uint32_t>(*md_profile_id)},
            ProfileId{static_cast<std::uint32_t>(*oe_profile_id)},
            *md_profile,
            *md_session,
            *oe_profile,
            *oe_session,
            *dictionary,
            *sender,
            *target,
            credential_reference,
        };
        venue_indexes.emplace(id, venues.size());
        venue_sources.emplace(id, section);
        venues.push_back(std::move(venue));
    }

    std::vector<RouteConfig> routes;
    std::set<std::pair<InstrumentId, VenueId>> route_keys;
    std::set<InstrumentId> mapped_instruments;
    std::set<VenueId> mapped_venues;
    for (const auto* section : route_sections) {
        const InstrumentId instrument{section->first_id};
        const VenueId venue{section->second_id};
        const auto instrument_it = instrument_indexes.find(instrument);
        const auto venue_it = venue_indexes.find(venue);
        if (instrument_it == instrument_indexes.end()) {
            add_error(
                result.errors,
                ConfigurationErrorCode::UnmappedReference,
                section->line,
                section->label,
                {},
                "route references an undefined instrument");
        }
        if (venue_it == venue_indexes.end()) {
            add_error(
                result.errors,
                ConfigurationErrorCode::UnmappedReference,
                section->line,
                section->label,
                {},
                "route references an undefined venue");
        }

        const auto wire_symbol =
            parse_identity_field(*section, "wire_symbol", result.errors);
        const auto price_numerator = parse_unsigned_field(
            *section, "price_numerator", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto price_denominator = parse_unsigned_field(
            *section, "price_denominator", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto quantity_numerator = parse_unsigned_field(
            *section, "quantity_numerator", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto quantity_denominator = parse_unsigned_field(
            *section, "quantity_denominator", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto aggressive =
            parse_bool_field(*section, "aggressive_ioc", result.errors);
        const auto passive =
            parse_bool_field(*section, "passive_gtc", result.errors);
        const auto allocation_cap = parse_unsigned_field(
            *section, "allocation_cap", 1,
            std::numeric_limits<std::uint64_t>::max(), result.errors);
        const auto aggressive_priority = parse_unsigned_field(
            *section, "aggressive_priority", 0,
            std::numeric_limits<std::uint32_t>::max(), result.errors);
        const auto passive_priority = parse_unsigned_field(
            *section, "passive_priority", 0,
            std::numeric_limits<std::uint32_t>::max(), result.errors);
        const auto configured_latency = parse_unsigned_field(
            *section, "configured_latency_ns", 0,
            std::numeric_limits<std::uint64_t>::max(), result.errors);

        if (!wire_symbol || !price_numerator || !price_denominator ||
            !quantity_numerator || !quantity_denominator ||
            !aggressive.has_value() || !passive.has_value() ||
            !allocation_cap || !aggressive_priority || !passive_priority ||
            !configured_latency || instrument_it == instrument_indexes.end() ||
            venue_it == venue_indexes.end()) {
            continue;
        }

        const auto price_conversion =
            RationalConversion::create(*price_numerator, *price_denominator);
        const auto quantity_conversion =
            RationalConversion::create(*quantity_numerator, *quantity_denominator);
        if (!price_conversion) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::Overflow,
                *section,
                "price_numerator",
                "price conversion cannot be represented safely");
        }
        if (!quantity_conversion) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::Overflow,
                *section,
                "quantity_numerator",
                "quantity conversion cannot be represented safely");
        }
        if (!price_conversion || !quantity_conversion) {
            continue;
        }
        if (!*aggressive && !*passive) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::InvalidValue,
                *section,
                "aggressive_ioc",
                "route must support at least one routing intent");
        }

        const auto& instrument_config = instruments[instrument_it->second];
        const auto& venue_config = venues[venue_it->second];
        const auto require_exact_price =
            [&](const Price value, const std::string_view field) {
                if (!price_conversion->convert_exact(value)) {
                    add_semantic_error(
                        result.errors,
                        ConfigurationErrorCode::InexactConversion,
                        *section,
                        field,
                        "configured canonical price is inexact for the venue conversion");
                }
            };
        const auto require_exact_quantity =
            [&](const Quantity value, const std::string_view field) {
                if (!quantity_conversion->convert_exact(value)) {
                    add_semantic_error(
                        result.errors,
                        ConfigurationErrorCode::InexactConversion,
                        *section,
                        field,
                        "configured canonical quantity is inexact for the venue conversion");
                }
            };
        require_exact_price(instrument_config.risk.minimum_price, "price_denominator");
        require_exact_price(instrument_config.risk.maximum_price, "price_denominator");
        require_exact_quantity(
            instrument_config.risk.maximum_parent_quantity, "quantity_denominator");
        require_exact_quantity(
            instrument_config.risk.maximum_child_quantity, "quantity_denominator");
        require_exact_quantity(
            instrument_config.risk.maximum_gross_outstanding, "quantity_denominator");
        require_exact_quantity(
            venue_config.risk.maximum_outstanding, "quantity_denominator");
        require_exact_quantity(Quantity{*allocation_cap}, "allocation_cap");

        if (*allocation_cap > instrument_config.risk.maximum_child_quantity.units) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::InvalidValue,
                *section,
                "allocation_cap",
                "route allocation cap exceeds instrument child risk limit");
        }
        if (*allocation_cap > venue_config.risk.maximum_outstanding.units) {
            add_semantic_error(
                result.errors,
                ConfigurationErrorCode::InvalidValue,
                *section,
                "allocation_cap",
                "route allocation cap exceeds venue outstanding limit");
        }
        if (!route_keys.emplace(instrument, venue).second) {
            add_error(
                result.errors,
                ConfigurationErrorCode::DuplicateIdentity,
                section->line,
                section->label,
                {},
                "instrument-to-venue route is duplicated");
        }

        routes.push_back(RouteConfig{
            venue,
            instrument,
            venue_config.name,
            instrument_config.canonical_symbol,
            *wire_symbol,
            *price_conversion,
            *quantity_conversion,
            VenueCapabilities{*aggressive, *passive},
            Quantity{*allocation_cap},
            static_cast<std::uint32_t>(*aggressive_priority),
            static_cast<std::uint32_t>(*passive_priority),
            *configured_latency,
            venue_config.admin_state,
        });
        mapped_instruments.insert(instrument);
        mapped_venues.insert(venue);
    }

    for (const auto& instrument : instruments) {
        if (!mapped_instruments.contains(instrument.id)) {
            const auto* section = instrument_sources.at(instrument.id);
            add_error(
                result.errors,
                ConfigurationErrorCode::UnmappedReference,
                section->line,
                section->label,
                {},
                "instrument has no valid venue route");
        }
    }
    for (const auto& venue : venues) {
        if (!mapped_venues.contains(venue.id)) {
            const auto* section = venue_sources.at(venue.id);
            add_error(
                result.errors,
                ConfigurationErrorCode::UnmappedReference,
                section->line,
                section->label,
                {},
                "venue has no valid instrument route");
        }
    }

    if (maximum_aggressive_venues &&
        *maximum_aggressive_venues > venues.size()) {
        add_semantic_error(
            result.errors,
            ConfigurationErrorCode::InvalidValue,
            *sorx_sections.front(),
            "maximum_aggressive_venues",
            "maximum aggressive venues exceeds configured venue count");
    }

    if (!routes.empty()) {
        RouteRib rib;
        std::vector<RouteValidationError> rib_errors;
        if (!rib.replace_all(routes, rib_errors)) {
            for (const auto& error : rib_errors) {
                add_error(
                    result.errors,
                    ConfigurationErrorCode::RouteValidation,
                    0,
                    "route-rib",
                    {},
                    error.message);
            }
        } else {
            routes.assign(rib.routes().begin(), rib.routes().end());
        }
    }

    std::sort(
        instruments.begin(),
        instruments.end(),
        [](const InstrumentConfiguration& lhs, const InstrumentConfiguration& rhs) {
            return lhs.id < rhs.id;
        });
    std::sort(
        venues.begin(),
        venues.end(),
        [](const VenueConfiguration& lhs, const VenueConfiguration& rhs) {
            return lhs.id < rhs.id;
        });

    if (!result.errors.empty() || !schema_version || !account_id ||
        !maximum_aggressive_venues || !ingress_capacity || !risk_generation) {
        return result;
    }

    RiskConfiguration risk;
    risk.generation = *risk_generation;
    risk.instruments.reserve(instruments.size());
    for (const auto& instrument : instruments) {
        risk.instruments.push_back(instrument.risk);
    }
    risk.venues.reserve(venues.size());
    for (const auto& venue : venues) {
        risk.venues.push_back(venue.risk);
    }

    ValidatedConfiguration configuration;
    configuration.schema_version = *schema_version;
    configuration.account_id = std::move(*account_id);
    configuration.ingress_capacity = *ingress_capacity;
    configuration.core = CoreConfiguration{
        std::move(risk),
        *maximum_aggressive_venues,
    };
    configuration.instruments = std::move(instruments);
    configuration.venues = std::move(venues);
    configuration.routes = std::move(routes);
    configuration.route_critical_hash =
        compute_route_critical_hash(configuration);
    configuration.route_critical_hash_hex =
        sha256_hex(configuration.route_critical_hash);
    result.configuration = std::move(configuration);
    return result;
}

ConfigurationResult ConfigurationLoader::load_ini_file(
    const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        ConfigurationResult result;
        add_error(
            result.errors,
            ConfigurationErrorCode::Io,
            0,
            path.string(),
            {},
            "unable to open configuration file");
        return result;
    }
    const std::string text{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
    if (!input.eof() && input.fail()) {
        ConfigurationResult result;
        add_error(
            result.errors,
            ConfigurationErrorCode::Io,
            0,
            path.string(),
            {},
            "failed while reading configuration file");
        return result;
    }
    return parse_ini(text);
}

Sha256Digest compute_route_critical_hash(
    const ValidatedConfiguration& configuration) {
    CanonicalWriter writer;
    writer.put_string("SORX_ROUTE_CRITICAL_TYPED_MODEL");
    writer.put_u32(1U);
    writer.put_u32(configuration.schema_version);
    writer.put_string(configuration.account_id);
    writer.put_u64(static_cast<std::uint64_t>(configuration.ingress_capacity));
    writer.put_u64(static_cast<std::uint64_t>(
        configuration.core.maximum_aggressive_venues));
    writer.put_u64(configuration.core.risk.generation);

    std::vector<const InstrumentConfiguration*> instruments;
    instruments.reserve(configuration.instruments.size());
    for (const auto& instrument : configuration.instruments) {
        instruments.push_back(&instrument);
    }
    std::sort(
        instruments.begin(),
        instruments.end(),
        [](const auto* lhs, const auto* rhs) { return lhs->id < rhs->id; });
    writer.put_u64(static_cast<std::uint64_t>(instruments.size()));
    for (const auto* instrument : instruments) {
        writer.put_u32(instrument->id.value());
        writer.put_string(instrument->canonical_symbol);
        writer.put_bool(instrument->risk.allowed);
        writer.put_u64(instrument->risk.maximum_parent_quantity.units);
        writer.put_u64(instrument->risk.maximum_child_quantity.units);
        writer.put_u64(instrument->risk.maximum_parent_notional.units);
        writer.put_u64(instrument->risk.maximum_child_notional.units);
        writer.put_i64(instrument->risk.minimum_price.units);
        writer.put_i64(instrument->risk.maximum_price.units);
        writer.put_u64(instrument->risk.maximum_open_parents);
        writer.put_u64(instrument->risk.maximum_open_children);
        writer.put_u64(instrument->risk.maximum_gross_outstanding.units);
    }

    std::vector<const VenueConfiguration*> venues;
    venues.reserve(configuration.venues.size());
    for (const auto& venue : configuration.venues) {
        venues.push_back(&venue);
    }
    std::sort(
        venues.begin(),
        venues.end(),
        [](const auto* lhs, const auto* rhs) { return lhs->id < rhs->id; });
    writer.put_u64(static_cast<std::uint64_t>(venues.size()));
    for (const auto* venue : venues) {
        writer.put_u32(venue->id.value());
        writer.put_string(venue->name);
        writer.put_u8(static_cast<std::uint8_t>(venue->admin_state));
        writer.put_bool(venue->risk.allowed);
        writer.put_u64(venue->risk.maximum_outstanding.units);
        writer.put_u64(static_cast<std::uint64_t>(
            venue->market_data_ingress_capacity));
        writer.put_u64(static_cast<std::uint64_t>(
            venue->order_entry_ingress_capacity));
        writer.put_u64(static_cast<std::uint64_t>(venue->cancel_egress_capacity));
        writer.put_u64(static_cast<std::uint64_t>(venue->normal_egress_capacity));
        writer.put_u32(venue->market_data_channel_id.value());
        writer.put_u32(venue->market_data_profile_id.value());
        writer.put_u32(venue->order_entry_profile_id.value());
        writer.put_string(venue->market_data_profile);
        writer.put_string(venue->market_data_session_identity);
        writer.put_string(venue->order_entry_profile);
        writer.put_string(venue->order_entry_session_identity);
        writer.put_string(venue->dictionary_identity);
        writer.put_string(venue->sender_comp_id);
        writer.put_string(venue->target_comp_id);
    }

    std::vector<const RouteConfig*> routes;
    routes.reserve(configuration.routes.size());
    for (const auto& route : configuration.routes) {
        routes.push_back(&route);
    }
    std::sort(
        routes.begin(),
        routes.end(),
        [](const auto* lhs, const auto* rhs) {
            return std::tie(lhs->instrument, lhs->venue, lhs->wire_symbol) <
                   std::tie(rhs->instrument, rhs->venue, rhs->wire_symbol);
        });
    writer.put_u64(static_cast<std::uint64_t>(routes.size()));
    for (const auto* route : routes) {
        writer.put_u32(route->instrument.value());
        writer.put_u32(route->venue.value());
        writer.put_string(route->canonical_symbol);
        writer.put_string(route->venue_name);
        writer.put_string(route->wire_symbol);
        writer.put_u64(route->price_conversion.numerator());
        writer.put_u64(route->price_conversion.denominator());
        writer.put_u64(route->quantity_conversion.numerator());
        writer.put_u64(route->quantity_conversion.denominator());
        writer.put_bool(route->capabilities.aggressive_ioc);
        writer.put_bool(route->capabilities.passive_gtc);
        writer.put_u64(route->allocation_cap.units);
        writer.put_u32(route->aggressive_priority);
        writer.put_u32(route->passive_priority);
        writer.put_u64(route->configured_latency_ns);
        writer.put_u8(static_cast<std::uint8_t>(route->admin_state));
    }

    Sha256 hash;
    hash.update(writer.bytes);
    return hash.finish();
}

std::string sha256_hex(const Sha256Digest& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        output.push_back(digits[(byte >> 4U) & 0x0fU]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

}  // namespace sorx::config


