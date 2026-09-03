#pragma once

#include <string>

namespace tether {

    // How the Bluetooth message journal and contact cache are kept on disk.
    enum class Retention {
        Encrypted, // AES-256-GCM, key in the desktop secret service
        Plaintext, // readable JSON
        None,      // nothing retained
    };

    const char* to_string(Retention retention);
    Retention retention_from_string(const std::string& value);

    namespace secret {

        // Set once at startup from the Bluetooth config, and again whenever the
        // user changes the mode. Defaults to Encrypted.
        void set_retention(Retention retention);
        Retention retention();

        // False when the wallet is locked or unreachable. Cached.
        bool have_key();

        inline constexpr int KEY_RETRY_SECONDS = 60;

        // base64(nonce || ciphertext || tag) for Encrypted, `plain` unchanged
        // under Plaintext. Empty when there is no key to seal with (do not write).
        std::string seal(const std::string& plain);

        // Inverse of seal, including the Plaintext pass-through. False when the
        // record is truncated, tampered with, or sealed under another key.
        bool unseal(const std::string& record, std::string& out);

        // Same, against a named mode rather than the active one.
        std::string seal(const std::string& plain, Retention mode);
        bool unseal(const std::string& record, std::string& out, Retention mode);

        // Forget the cached key and retention. Tests only.
        void reset_for_test();

    } // namespace secret

} // namespace tether
