// realmheart-auth-helper — setuid-root PAM verifier for the Broken Seal
// lockscreen. The shell runs as the user, which cannot read /etc/shadow;
// pam_unix needs root for that. This tiny helper is installed setuid-root and
// performs the PAM check on the shell's behalf.
//
// Protocol: argv[1] = username, stdin = password (single line). Exits 0 on
// success, non-zero on failure. No output.

#include <security/pam_appl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int pam_conversation(
    int num_msg,
    const struct pam_message** msg,
    struct pam_response** resp,
    void* appdata_ptr
) {
    auto* password = static_cast<const std::string*>(appdata_ptr);
    if (num_msg <= 0 || msg == nullptr || resp == nullptr || password == nullptr) {
        return PAM_CONV_ERR;
    }
    auto* responses = static_cast<struct pam_response*>(
        calloc(static_cast<std::size_t>(num_msg), sizeof(struct pam_response))
    );
    if (responses == nullptr) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; ++i) {
        if (msg[i] == nullptr || msg[i]->msg_style != PAM_PROMPT_ECHO_OFF) {
            free(responses);
            return PAM_CONV_ERR;
        }
        responses[i].resp = strdup(password->c_str());
        if (responses[i].resp == nullptr) {
            free(responses);
            return PAM_BUF_ERR;
        }
    }
    *resp = responses;
    return PAM_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || argv[1] == nullptr || *argv[1] == '\0') return 2;

    // Read the password from stdin (single line).
    std::string password;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), stdin) != nullptr) {
        password += buffer;
        if (!strchr(buffer, '\n')) break;
    }
    // Strip trailing newline.
    while (!password.empty() &&
           (password.back() == '\n' || password.back() == '\r')) {
        password.pop_back();
    }
    if (password.empty()) return 2;

    struct pam_conv conv{pam_conversation, &password};
    pam_handle_t* handle = nullptr;
    int ret = pam_start("realmheart-lockscreen", argv[1], &conv, &handle);
    if (ret == PAM_SUCCESS) {
        ret = pam_authenticate(handle, 0);
        if (ret == PAM_SUCCESS) ret = pam_acct_mgmt(handle, 0);
        pam_end(handle, ret);
    }
    return ret == PAM_SUCCESS ? 0 : 1;
}
