#include "services/ProphecyLayoutEngine.hpp"
#include "services/ProphecyMotionEngine.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

// Forward declarations for PAM.
extern "C" {
#include <security/pam_appl.h>
}

namespace {

// Simple PAM conversation function for password prompts.
struct PamResponseData {
    std::string password;
    bool got_response = false;
};

int pam_conversation(int num_msg, const struct pam_message** msg,
                     struct pam_response** resp, void* appdata) {
    auto* data = static_cast<PamResponseData*>(appdata);
    struct pam_response* response =
        static_cast<struct pam_response*>(calloc(num_msg, sizeof(struct pam_response)));
    if (!response) return PAM_BUF_ERR;

    for (int i = 0; i < num_msg; ++i) {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            response[i].resp = strdup(data->password.c_str());
            data->got_response = true;
        }
    }
    *resp = response;
    return PAM_SUCCESS;
}

// Read a line from fd 3 (the control socket from parent).
bool read_command(int fd, std::string& out_command) {
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return false;
    buf[n] = '\0';
    out_command = std::string(buf);
    return true;
}

void send_response(int fd, const std::string& response) {
    std::string msg = response + "\n";
    write(fd, msg.data(), msg.size());
}

} // namespace

int main(int argc, char** argv) {
    // Parse arguments: --stdio --socket-fd <N> --seed <N> --background <path> --rinia <path>
    int socket_fd = 3;
    std::uint64_t seed = 0;
    std::string background_asset;
    std::string rinia_asset;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--stdio") continue;
        if (arg == "--socket-fd" && i + 1 < argc) socket_fd = std::stoi(argv[++i]);
        if (arg == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
        if (arg == "--background" && i + 1 < argc) background_asset = argv[++i];
        if (arg == "--rinia" && i + 1 < argc) rinia_asset = argv[++i];
    }

    // Send READY handshake to parent.
    send_response(socket_fd, "READY");

    // Initialize animation engines.
    realmheart::services::ProphecyMotionEngine motion_engine;
    motion_engine.reset();

    // Compute the prophecy layout from the seed.
    auto layout = realmheart::services::ProphecyLayoutEngine::compute(
        seed, 6  // 6 futures for full prophecy spread
    );
    assert(layout.futures.size() > 0 && "layout must have futures");

    // Authentication state.
    bool authenticated = false;
    int auth_failures = 0;
    PamResponseData pam_data;

    // Main loop: wait for commands from parent, render frames.
    // In a full implementation, this would run a Wayland/EGL render loop
    // using the ext-session-lock-v1 surfaces passed from the parent.
    // For Phase 4, we stub the rendering and focus on PAM auth flow.

    while (!authenticated) {
        // Process pending commands from parent (non-blocking read).
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 16000;  // ~60fps

        int ret = select(socket_fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ret > 0 && FD_ISSET(socket_fd, &read_fds)) {
            std::string command;
            if (read_command(socket_fd, command)) {
                if (command.find("AUTH") == 0) {
                    // AUTH <password> — verify via PAM.
                    std::string password = command.substr(5);  // skip "AUTH "

                    pam_data.password = password;
                    pam_data.got_response = false;

                    pam_handle_t* pamh = nullptr;
                    struct pam_conv conv = {pam_conversation, &pam_data};

                    int pam_result = pam_start("realmheart-lock", nullptr, &conv, &pamh);
                    if (pam_result == PAM_SUCCESS) {
                        pam_result = pam_authenticate(pamh, 0);
                    }
                    if (pam_result == PAM_SUCCESS) {
                        pam_result = pam_acct_mgmt(pamh, 0);
                    }

                    if (pam_result == PAM_SUCCESS) {
                        authenticated = true;
                        pam_end(pamh, pam_result);
                        send_response(socket_fd, "UNLOCK");
                    } else {
                        auth_failures++;
                        pam_end(pamh, pam_result);

                        if (auth_failures >= 3) {
                            send_response(socket_fd, "ERROR max_auth_failures_exceeded");
                        } else {
                            send_response(socket_fd, "AUTH_FAIL");
                        }
                    }
                } else if (command.find("PING") == 0) {
                    send_response(socket_fd, "PONG");
                } else if (command.find("QUIT") == 0) {
                    break;
                } else if (command.find("STATE ") == 0) {
                    // Future: handle state updates from parent.
                }
            } else {
                // Parent closed the connection.
                break;
            }
        }

        // Update animation state (would render to lock surfaces in full impl).
        realmheart::services::ProphecyMotionEngine::MotionState state;
        motion_engine.update(0.016f, state);
    }

    return authenticated ? 0 : 1;
}
