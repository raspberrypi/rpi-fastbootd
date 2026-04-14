/*
 * Shared IDP context for parallel TCP partition provisioning.
 *
 * When multiple TCP connections flash partitions concurrently, they share
 * a single IDPdevice and use mutex-protected partition claiming via the
 * cookie iterator. One "control" connection initialises/finalises, while
 * all connections can claim and flash partitions in parallel.
 */
#pragma once

#include <memory>
#include <mutex>
#include <optional>

#include "idpdevice.h"

class SharedIDPContext {
  public:
    SharedIDPContext() = default;
    ~SharedIDPContext() = default;

    // Initialize IDP from staged JSON data.
    // Returns false if already initialised or if the description is invalid.
    bool Initialize(const char* data, size_t length) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (idp_) return false;  // Already initialised

        auto dev = std::make_unique<IDPdevice>();
        if (!dev->Initialise(data, length)) {
            return false;
        }
        std::string reason;
        if (!dev->canProvision(reason)) {
            return false;
        }

        idp_ = std::move(dev);
        cookie_ = idp_->createCookie();
        return true;
    }

    // Start provisioning (creates partition table etc).
    bool StartProvision() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idp_) return false;
        std::string reason;
        return idp_->startProvision(reason);
    }

    // Thread-safe: claim the next partition to flash.
    // Returns nullopt when all partitions have been claimed.
    std::optional<IDPblockDevice> ClaimNextPartition() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idp_ || !cookie_) return std::nullopt;
        return idp_->getNextBlockDevice(*cookie_);
    }

    // Finalise provisioning and tear down the IDP state.
    bool EndProvision() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!idp_) return true;
        bool result = idp_->endProvision();
        idp_.reset();
        cookie_.reset();
        return result;
    }

  private:
    mutable std::mutex mutex_;
    std::unique_ptr<IDPdevice> idp_;
    IDPdevice::CookiePtr cookie_{nullptr, IDPcookieDeleter};
};
