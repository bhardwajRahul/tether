#pragma once

#include <filesystem>
#include <gio/gio.h>
#include <string>

namespace tether::bluetooth {

    // obexd removes a transfer object as soon as it finishes, so an object that
    // has disappeared is a normal terminal state rather than a failure.
    enum class TransferState { Active, Complete, Error, Gone };

    // Polls a transfer to completion. Returns Active only when the timeout ran
    // out with the transfer still in progress.
    TransferState wait_for_transfer(GDBusConnection* bus, const std::string& path, int timeout_seconds);

    // Where to stage a file obexd reads or writes.
    std::filesystem::path obex_staging_path(const std::string& name);

} // namespace tether::bluetooth
