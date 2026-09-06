// realmheart-auth-helper — setuid-root PAM verifier for the
// lockscreen. The shell runs as the user, which cannot read /etc/shadow;
// pam_unix needs root for that. This tiny helper is installed setuid-root and
// performs the PAM check on the shell's behalf.
//
// Protocol: argv[1] = username, stdin = password (single line). Exits 0 on
// success, non-zero on failure. No output.

#include <security/pam_appl.h>

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace {

constexpr std::size_t kMaxPasswordBytes = 512;
constexpr unsigned int kOperationTimeoutSeconds = 10;

[[noreturn]] void operation_timeout(int) noexcept {
    _exit(124);
}

void wipe(char* data, const std::size_t size) noexcept {
    if (data == nullptr) return;
    volatile char* cursor = data;
    for (std::size_t index = 0; index < size; ++index) {
        cursor[index] = '\0';
    }
}

struct Password {
    std::array<char, kMaxPasswordBytes + 1> bytes{};
    std::size_t size = 0;

    ~Password() { wipe(bytes.data(), bytes.size()); }

    [[nodiscard]] const char* data() const noexcept { return bytes.data(); }
};

bool read_password(Password& password) noexcept {
    bool newline_seen = false;
    while (!newline_seen) {
        char byte = '\0';
        const ssize_t count = ::read(STDIN_FILENO, &byte, 1);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (byte == '\n') {
            newline_seen = true;
            break;
        }
        if (password.size >= kMaxPasswordBytes) return false;
        password.bytes[password.size++] = byte;
    }

    while (password.size > 0 && password.bytes[password.size - 1] == '\r') {
        --password.size;
    }
    password.bytes[password.size] = '\0';
    return newline_seen && password.size > 0;
}

bool caller_username(char* buffer, const std::size_t buffer_size,
                     std::string_view& username) noexcept {
    struct passwd entry{};
    struct passwd* result = nullptr;
    const int status = ::getpwuid_r(
        ::getuid(), &entry, buffer, buffer_size, &result
    );
    if (status != 0 || result == nullptr || result->pw_name == nullptr ||
        *result->pw_name == '\0') {
        return false;
    }
    username = result->pw_name;
    return true;
}

int pam_conversation(
    int num_msg,
    const struct pam_message** msg,
    struct pam_response** resp,
    void* appdata_ptr
) {
    auto* password = static_cast<const Password*>(appdata_ptr);
    if (num_msg <= 0 || num_msg > 32 || msg == nullptr || resp == nullptr ||
        password == nullptr) {
        return PAM_CONV_ERR;
    }

    auto* responses = static_cast<struct pam_response*>(
        calloc(static_cast<std::size_t>(num_msg), sizeof(struct pam_response))
    );
    if (responses == nullptr) return PAM_BUF_ERR;

    const auto cleanup = [&](const int count) noexcept {
        for (int index = 0; index < count; ++index) {
            if (responses[index].resp != nullptr) {
                wipe(responses[index].resp, password->size + 1);
                free(responses[index].resp);
            }
        }
        free(responses);
    };

    for (int index = 0; index < num_msg; ++index) {
        if (msg[index] == nullptr ||
            (msg[index]->msg_style != PAM_PROMPT_ECHO_OFF &&
             msg[index]->msg_style != PAM_TEXT_INFO &&
             msg[index]->msg_style != PAM_ERROR_MSG)) {
            cleanup(index);
            return PAM_CONV_ERR;
        }

        if (msg[index]->msg_style == PAM_TEXT_INFO ||
            msg[index]->msg_style == PAM_ERROR_MSG) {
            continue;
        }

        responses[index].resp = static_cast<char*>(
            calloc(password->size + 1, sizeof(char))
        );
        if (responses[index].resp == nullptr) {
            cleanup(index);
            return PAM_BUF_ERR;
        }
        if (password->size > 0) {
            // PAM owns this response array after the callback returns.
            std::memcpy(
                responses[index].resp, password->data(), password->size
            );
        }
    }

    *resp = responses;
    return PAM_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || argv[1] == nullptr || *argv[1] == '\0') return 2;

    std::signal(SIGALRM, operation_timeout);
    ::alarm(kOperationTimeoutSeconds);

    // A direct invocation must be unable to query any account other than the
    // caller's real UID. The effective UID is intentionally root after setuid.
    std::array<char, 16384> user_buffer{};
    std::string_view caller;
    if (!caller_username(user_buffer.data(), user_buffer.size(), caller) ||
        caller != argv[1]) {
        return 3;
    }

    Password password;
    if (!read_password(password)) return 2;

    struct pam_conv conv{pam_conversation, &password};
    pam_handle_t* handle = nullptr;
    int ret = pam_start("realmheart-lockscreen", argv[1], &conv, &handle);
    if (ret == PAM_SUCCESS) {
        ret = pam_authenticate(handle, 0);
        if (ret == PAM_SUCCESS) {
            ret = pam_acct_mgmt(handle, 0);
        }
        pam_end(handle, ret);
    }
    ::alarm(0);
    return ret == PAM_SUCCESS ? 0 : 1;
}
