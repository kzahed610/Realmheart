#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace realmheart::worldscar {

enum class WorldscarResultKind {
    Cancel,
    // First half of the apply handshake: start preparing the selected real
    // wallpaper while Worldscar collapses into its residual slash.
    Apply,
    // Worldscar has reached the residual slash and the real wallpaper backend
    // has reported PREPARED. The shell may now commit the hidden texture.
    Commit,
    // Backend reveal finished and Worldscar has faded its final damage line.
    Complete,
    Error,
};

struct WorldscarResult {
    WorldscarResultKind kind = WorldscarResultKind::Cancel;
    std::string payload;
};

enum class WorldscarCommandKind {
    Prepare,
    Open,
    Close,
    ApplyPrepared,
    ApplyCommitted,
    ApplyFailed,
    Refresh,
};

struct WorldscarCommand {
    WorldscarCommandKind kind = WorldscarCommandKind::Close;
    std::string payload;
};

[[nodiscard]] std::string serialize_worldscar_result(
    const WorldscarResult& result
);

[[nodiscard]] std::optional<WorldscarResult> parse_worldscar_result(
    std::string_view line
);

[[nodiscard]] std::string serialize_worldscar_command(
    const WorldscarCommand& command
);

[[nodiscard]] std::optional<WorldscarCommand> parse_worldscar_command(
    std::string_view line
);

} // namespace realmheart::worldscar
