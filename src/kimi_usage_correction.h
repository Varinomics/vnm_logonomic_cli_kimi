#pragma once

#include <logonomic_cli_addon/usage_owned_event_correction.h>

namespace kimi {

// The caller supplies only occurrences whose accepted raw prefix and original
// owner have been proved. A null result means the host's retained partition
// cannot be mapped without guessing; it grants no cache mode transition.
std::optional<logonomic::cli_addon::Usage_record> adjust_owned_usage(
    const logonomic::cli_addon::Usage_owned_event_predecessor& predecessor,
    const std::optional<logonomic::cli_addon::Usage_record>& accepted_files,
    const std::optional<logonomic::cli_addon::Usage_record>& removed,
    const std::optional<logonomic::cli_addon::Usage_record>& growth,
    const std::vector<std::string>& original_record_ids);

logonomic::cli_addon::Usage_record retain_usage_predecessor(
    const logonomic::cli_addon::Usage_owned_event_predecessor& predecessor);

} // namespace kimi
