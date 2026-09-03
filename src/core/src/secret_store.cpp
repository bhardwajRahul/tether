#include "tether/secret_store.hpp"
#include "tether/base64.hpp"
#include "tether/log.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <libsecret/secret.h>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <pwd.h>
#include <unistd.h>

namespace tether {

    const char* to_string(Retention retention) {
        switch (retention) {
        case Retention::Plaintext:
            return "plaintext";
        case Retention::None:
            return "none";
        case Retention::Encrypted:
            break;
        }
        return "encrypted";
    }

    Retention retention_from_string(const std::string& value) {
        if (value == "plaintext")
            return Retention::Plaintext;
        if (value == "none")
            return Retention::None;
        return Retention::Encrypted;
    }

    namespace secret {

        namespace {

            constexpr size_t KEY_BYTES = 32;
            constexpr size_t NONCE_BYTES = 12;
            constexpr size_t TAG_BYTES = 16;

            const SecretSchema* store_schema() {
                static const SecretSchema schema = {
                    "com.tether.Store",
                    SECRET_SCHEMA_NONE,
                    {
                        {"application", SECRET_SCHEMA_ATTRIBUTE_STRING},
                        {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
                    },
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                };
                return &schema;
            }

            std::string home_dir() {
                if (const char* home = getenv("HOME"); home && *home)
                    return home;
                if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
                    return pw->pw_dir;
                return {};
            }

            std::string keyfile_path() {
                std::string home = home_dir();
                if (home.empty())
                    return {};
                return (std::filesystem::path(home) / ".config" / "tether" / "store.key").string();
            }

            int64_t now_seconds() {
                return std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            }

            std::mutex g_mutex;
            Retention g_retention = Retention::Encrypted;
            std::vector<unsigned char> g_key;
            int64_t g_last_failure = 0;
            bool g_failed_once = false;

            // Reads the key out of the desktop secret service without unlocking it.
            bool key_from_service(std::vector<unsigned char>& out, bool& service_present) {
                service_present = false;

                GError* err = nullptr;
                SecretService* service = secret_service_get_sync(SECRET_SERVICE_OPEN_SESSION, nullptr, &err);
                if (!service) {
                    if (err) {
                        debug::log(DEBUG, "secret: no secret service on the bus: {}", err->message);
                        g_error_free(err);
                    }
                    return false;
                }
                service_present = true;

                GHashTable* attributes = g_hash_table_new(g_str_hash, g_str_equal);
                g_hash_table_insert(attributes, const_cast<char*>("application"), const_cast<char*>("tether"));
                GList* items = secret_service_search_sync(
                    service, store_schema(), attributes, SECRET_SEARCH_LOAD_SECRETS, nullptr, &err);
                g_hash_table_unref(attributes);
                g_object_unref(service);

                if (err) {
                    debug::log(WARN, "secret: search failed: {}", err->message);
                    g_error_free(err);
                    return false;
                }

                bool found = false;
                for (GList* node = items; node && !found; node = node->next) {
                    auto* item = static_cast<SecretItem*>(node->data);
                    // null on a locked collection
                    SecretValue* value = secret_item_get_secret(item);
                    if (!value)
                        continue;
                    gsize length = 0;
                    const gchar* text = secret_value_get(value, &length);
                    if (text) {
                        auto decoded = base64_decode(std::string(text, length));
                        if (decoded.size() == KEY_BYTES) {
                            out = std::move(decoded);
                            found = true;
                        }
                    }
                    secret_value_unref(value);
                }
                g_list_free_full(items, g_object_unref);
                return found;
            }

            bool key_to_service(const std::vector<unsigned char>& key) {
                GError* err = nullptr;
                const std::string encoded = base64_encode(key);
                const gboolean ok = secret_password_store_sync(store_schema(),
                                                               SECRET_COLLECTION_DEFAULT,
                                                               "Tether message store key",
                                                               encoded.c_str(),
                                                               nullptr,
                                                               &err,
                                                               "application",
                                                               "tether",
                                                               nullptr);
                if (err) {
                    debug::log(WARN, "secret: cannot store the key in the wallet: {}", err->message);
                    g_error_free(err);
                }
                return ok == TRUE;
            }

            bool key_from_file(std::vector<unsigned char>& out) {
                const std::string path = keyfile_path();
                if (path.empty())
                    return false;
                std::ifstream in(path, std::ios::binary);
                if (!in.is_open())
                    return false;
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                auto decoded = base64_decode(text);
                if (decoded.size() != KEY_BYTES)
                    return false;
                out = std::move(decoded);
                return true;
            }

            bool key_to_file(const std::vector<unsigned char>& key) {
                const std::string path = keyfile_path();
                if (path.empty())
                    return false;

                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

                const std::string tmp = path + ".tmp";
                {
                    std::ofstream out(tmp, std::ios::trunc);
                    if (!out.is_open()) {
                        debug::log(ERR, "secret: cannot write {}", tmp);
                        return false;
                    }
                    out << base64_encode(key) << "\n";
                    if (!out)
                        return false;
                }
                std::filesystem::permissions(tmp,
                                             std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                             std::filesystem::perm_options::replace,
                                             ec);
                std::filesystem::rename(tmp, path, ec);
                if (ec) {
                    debug::log(ERR, "secret: cannot replace {}: {}", path, ec.message());
                    std::filesystem::remove(tmp, ec);
                    return false;
                }
                return true;
            }

            // Caller holds g_mutex.
            bool load_key_locked() {
                if (!g_key.empty())
                    return true;
                if (g_failed_once && now_seconds() - g_last_failure < KEY_RETRY_SECONDS)
                    return false;

                bool service_present = false;
                std::vector<unsigned char> key;
                if (key_from_service(key, service_present)) {
                    g_key = std::move(key);
                    g_failed_once = false;
                    return true;
                }

                if (!service_present && key_from_file(key)) {
                    g_key = std::move(key);
                    g_failed_once = false;
                    return true;
                }

                if (service_present) {
                    // wallet is locked, or this is first run
                    std::vector<unsigned char> fresh(KEY_BYTES);
                    if (RAND_bytes(fresh.data(), static_cast<int>(fresh.size())) != 1)
                        return false;
                    if (key_to_service(fresh)) {
                        g_key = std::move(fresh);
                        g_failed_once = false;
                        debug::log(INFO, "secret: generated a new store key in the wallet");
                        return true;
                    }
                    debug::log(WARN, "secret: wallet is locked; retained history is paused until it unlocks");
                    g_failed_once = true;
                    g_last_failure = now_seconds();
                    return false;
                }

                std::vector<unsigned char> fresh(KEY_BYTES);
                if (RAND_bytes(fresh.data(), static_cast<int>(fresh.size())) != 1)
                    return false;
                if (!key_to_file(fresh)) {
                    g_failed_once = true;
                    g_last_failure = now_seconds();
                    return false;
                }
                g_key = std::move(fresh);
                g_failed_once = false;
                debug::log(INFO, "secret: no secret service; store key kept in a 0600 file");
                return true;
            }

            bool encrypt(const std::string& plain,
                         const std::vector<unsigned char>& key,
                         std::vector<unsigned char>& out) {
                out.resize(NONCE_BYTES + plain.size() + TAG_BYTES);
                if (RAND_bytes(out.data(), NONCE_BYTES) != 1)
                    return false;

                EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
                if (!ctx)
                    return false;

                bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), out.data()) == 1;
                int length = 0;
                if (ok)
                    ok = EVP_EncryptUpdate(ctx,
                                           out.data() + NONCE_BYTES,
                                           &length,
                                           reinterpret_cast<const unsigned char*>(plain.data()),
                                           static_cast<int>(plain.size())) == 1;
                int total = length;
                if (ok)
                    ok = EVP_EncryptFinal_ex(ctx, out.data() + NONCE_BYTES + total, &length) == 1;
                total += length;
                if (ok)
                    ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, TAG_BYTES, out.data() + NONCE_BYTES + total) ==
                         1;
                EVP_CIPHER_CTX_free(ctx);

                if (!ok)
                    return false;
                out.resize(NONCE_BYTES + static_cast<size_t>(total) + TAG_BYTES);
                return true;
            }

            bool decrypt(const std::vector<unsigned char>& sealed,
                         const std::vector<unsigned char>& key,
                         std::string& out) {
                if (sealed.size() < NONCE_BYTES + TAG_BYTES)
                    return false;
                const size_t body = sealed.size() - NONCE_BYTES - TAG_BYTES;

                EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
                if (!ctx)
                    return false;

                std::string plain(body, '\0');
                bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), sealed.data()) == 1;
                int length = 0;
                if (ok && body)
                    ok = EVP_DecryptUpdate(ctx,
                                           reinterpret_cast<unsigned char*>(plain.data()),
                                           &length,
                                           sealed.data() + NONCE_BYTES,
                                           static_cast<int>(body)) == 1;
                int total = length;
                if (ok)
                    ok = EVP_CIPHER_CTX_ctrl(ctx,
                                             EVP_CTRL_GCM_SET_TAG,
                                             TAG_BYTES,
                                             const_cast<unsigned char*>(sealed.data() + NONCE_BYTES + body)) == 1;
                // Fails when the tag does not match, which is the whole point.
                if (ok)
                    ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + total, &length) == 1;
                total += length;
                EVP_CIPHER_CTX_free(ctx);

                if (!ok)
                    return false;
                plain.resize(static_cast<size_t>(total));
                out = std::move(plain);
                return true;
            }

        } // namespace

        void set_retention(Retention retention) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_retention = retention;
        }

        Retention retention() {
            std::lock_guard<std::mutex> lock(g_mutex);
            return g_retention;
        }

        bool have_key() {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_retention != Retention::Encrypted)
                return true;
            return load_key_locked();
        }

        std::string seal(const std::string& plain, Retention mode) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (mode == Retention::Plaintext)
                return plain;
            if (mode == Retention::None || !load_key_locked())
                return {};

            std::vector<unsigned char> sealed;
            if (!encrypt(plain, g_key, sealed)) {
                debug::log(ERR, "secret: failed to seal a record");
                return {};
            }
            return base64_encode(sealed);
        }

        bool unseal(const std::string& record, std::string& out, Retention mode) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (mode == Retention::Plaintext) {
                out = record;
                return true;
            }
            if (mode == Retention::None || !load_key_locked())
                return false;
            return decrypt(base64_decode(record), g_key, out);
        }

        std::string seal(const std::string& plain) { return seal(plain, retention()); }

        bool unseal(const std::string& record, std::string& out) { return unseal(record, out, retention()); }

        void reset_for_test() {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_retention = Retention::Encrypted;
            g_key.clear();
            g_failed_once = false;
            g_last_failure = 0;
        }

    } // namespace secret

} // namespace tether
