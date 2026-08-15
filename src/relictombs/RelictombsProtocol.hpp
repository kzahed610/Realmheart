#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace realmheart::relictombs {

enum class RelictombsResultKind {
    Cancel,
    // First half of the apply handshake: start preparing the selected real
    // wallpaper while Relictombs collapses into its residual slash.
    Apply,
    // Relictombs has reached the residual slash and the real wallpaper backend
    // has reported PREPARED. The shell may now commit the hidden texture.
    Commit,
    // Backend reveal finished and Relictombs has faded its final damage line.
    Complete,
    Error,
};

struct RelictombsResult {
    RelictombsResultKind kind = RelictombsResultKind::Cancel;
    std::string payload;
};

enum class RelictombsCommandKind {
    Prepare,
    Open,
    Close,
    ApplyPrepared,
    ApplyCommitted,
    ApplyFailed,
    Refresh,
};

struct RelictombsCommand {
    RelictombsCommandKind kind = RelictombsCommandKind::Close;
    std::string payload;
};

[[nodiscard]] std::string serialize_relictombs_result(
    const RelictombsResult& result
);

[[nodiscard]] std::optional<RelictombsResult> parse_relictombs_result(
    std::string_view line
);

[[nodiscard]] std::string serialize_relictombs_command(
    const RelictombsCommand& command
);

[[nodiscard]] std::optional<RelictombsCommand> parse_relictombs_command(
    std::string_view line
);

} // namespace realmheart::relictombs
