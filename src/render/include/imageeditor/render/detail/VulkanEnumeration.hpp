#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace imageeditor::render::detail {

inline constexpr std::uint32_t kVulkanEnumerationMaxAttempts = 8;

template<typename Value>
struct VulkanEnumerationResult {
    VkResult result {VK_SUCCESS};
    std::vector<Value> values;
    std::uint32_t attempts {0};
};

template<typename Value>
struct VulkanPropertyEnumerationResult {
    bool complete {false};
    std::vector<Value> values;
    std::uint32_t attempts {0};
};

[[nodiscard]] constexpr std::uint32_t growEnumerationCapacity(
    std::uint32_t capacity) noexcept
{
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (capacity == 0) {
        return 1;
    }
    if (capacity > maximum / 2U) {
        return maximum;
    }
    return capacity * 2U;
}

// Vulkan's count-then-data enumeration idiom is a snapshot, not an atomic
// operation. The second call may legally return VK_INCOMPLETE when the list
// grows between calls. Re-querying alone can repeatedly allocate exactly one
// stale count, so retries also retain geometric spare capacity.
template<typename Value, typename Query>
[[nodiscard]] VulkanEnumerationResult<Value> enumerateVulkanValues(
    Query&& query,
    std::uint32_t maxAttempts = kVulkanEnumerationMaxAttempts)
{
    VulkanEnumerationResult<Value> outcome;
    std::uint32_t capacityHint = 0;
    const auto attempts = std::max(maxAttempts, 1U);

    for (std::uint32_t attempt = 1; attempt <= attempts; ++attempt) {
        outcome.attempts = attempt;

        std::uint32_t reportedCount = 0;
        const auto countResult = query(
            &reportedCount, static_cast<Value*>(nullptr));
        if (countResult != VK_SUCCESS && countResult != VK_INCOMPLETE) {
            outcome.result = countResult;
            outcome.values.clear();
            return outcome;
        }
        if (countResult == VK_SUCCESS && reportedCount == 0) {
            outcome.result = VK_SUCCESS;
            outcome.values.clear();
            return outcome;
        }

        const auto capacity = std::max(reportedCount, capacityHint);
        outcome.values.resize(static_cast<std::size_t>(
            capacity == 0 ? 1U : capacity));
        auto writtenCount = static_cast<std::uint32_t>(outcome.values.size());
        const auto valuesResult = query(&writtenCount, outcome.values.data());
        if (valuesResult == VK_SUCCESS) {
            // Vulkan requires writtenCount to be no greater than the supplied
            // capacity on success. Preserve an explicit failure if a broken
            // implementation violates that contract.
            if (static_cast<std::size_t>(writtenCount) > outcome.values.size()) {
                outcome.result = VK_ERROR_UNKNOWN;
                outcome.values.clear();
                return outcome;
            }
            outcome.values.resize(static_cast<std::size_t>(writtenCount));
            outcome.result = VK_SUCCESS;
            return outcome;
        }
        if (valuesResult != VK_INCOMPLETE) {
            outcome.result = valuesResult;
            outcome.values.clear();
            return outcome;
        }

        const auto observedCapacity = std::max(
            static_cast<std::uint32_t>(outcome.values.size()), writtenCount);
        capacityHint = growEnumerationCapacity(observedCapacity);
        outcome.values.clear();
        outcome.result = VK_INCOMPLETE;
    }

    return outcome;
}

// A few older Vulkan property queries use the same count-then-data shape but
// return void, so they cannot report VK_INCOMPLETE. Compare a fresh count after
// the data call and retry if the supplied array may have been truncated.
template<typename Value, typename Query>
[[nodiscard]] VulkanPropertyEnumerationResult<Value> enumerateVulkanProperties(
    Query&& query,
    std::uint32_t maxAttempts = kVulkanEnumerationMaxAttempts)
{
    VulkanPropertyEnumerationResult<Value> outcome;
    std::uint32_t capacityHint = 0;
    const auto attempts = std::max(maxAttempts, 1U);

    for (std::uint32_t attempt = 1; attempt <= attempts; ++attempt) {
        outcome.attempts = attempt;

        std::uint32_t reportedCount = 0;
        query(&reportedCount, static_cast<Value*>(nullptr));
        if (reportedCount == 0) {
            outcome.complete = true;
            outcome.values.clear();
            return outcome;
        }

        const auto capacity = std::max(reportedCount, capacityHint);
        outcome.values.resize(static_cast<std::size_t>(capacity));
        auto writtenCount = capacity;
        query(&writtenCount, outcome.values.data());
        if (static_cast<std::size_t>(writtenCount) > outcome.values.size()) {
            outcome.complete = false;
            outcome.values.clear();
            return outcome;
        }

        std::uint32_t latestCount = 0;
        query(&latestCount, static_cast<Value*>(nullptr));
        if (latestCount <= writtenCount) {
            outcome.values.resize(static_cast<std::size_t>(writtenCount));
            outcome.complete = true;
            return outcome;
        }

        const auto observedCapacity = std::max(capacity, latestCount);
        capacityHint = growEnumerationCapacity(observedCapacity);
        outcome.values.clear();
    }

    return outcome;
}

} // namespace imageeditor::render::detail
