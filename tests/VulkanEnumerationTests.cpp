#include "imageeditor/render/detail/VulkanEnumeration.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::cerr << "FAIL line " << line << ": " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using imageeditor::render::detail::enumerateVulkanValues;
using imageeditor::render::detail::enumerateVulkanProperties;

void stableEnumerationCompletesInOneAttempt()
{
    const auto result = enumerateVulkanValues<int>(
        [](std::uint32_t* count, int* values) {
            if (!values) {
                *count = 3;
                return VK_SUCCESS;
            }
            CHECK(*count >= 3);
            values[0] = 10;
            values[1] = 20;
            values[2] = 30;
            *count = 3;
            return VK_SUCCESS;
        });

    CHECK(result.result == VK_SUCCESS);
    CHECK(result.attempts == 1);
    CHECK(result.values == std::vector<int>({10, 20, 30}));
}

void incompleteResultRequeriesAndRecovers()
{
    int dataCalls = 0;
    const auto result = enumerateVulkanValues<int>(
        [&dataCalls](std::uint32_t* count, int* values) {
            if (!values) {
                *count = dataCalls == 0 ? 2U : 3U;
                return VK_SUCCESS;
            }
            ++dataCalls;
            if (dataCalls == 1) {
                CHECK(*count >= 2);
                values[0] = 1;
                values[1] = 2;
                *count = 2;
                return VK_INCOMPLETE;
            }
            CHECK(*count >= 3);
            values[0] = 1;
            values[1] = 2;
            values[2] = 3;
            *count = 3;
            return VK_SUCCESS;
        });

    CHECK(result.result == VK_SUCCESS);
    CHECK(result.attempts == 2);
    CHECK(result.values == std::vector<int>({1, 2, 3}));
}

void geometricHeadroomEscapesARepeatedStaleCount()
{
    constexpr std::uint32_t actualCount = 5;
    const auto result = enumerateVulkanValues<int>(
        [actualCount](std::uint32_t* count, int* values) {
            if (!values) {
                // Model a count query that repeatedly observes an older
                // surface-format snapshot.
                *count = 2;
                return VK_SUCCESS;
            }
            const auto capacity = *count;
            const auto written = std::min(capacity, actualCount);
            for (std::uint32_t index = 0; index < written; ++index) {
                values[index] = static_cast<int>(index + 1U);
            }
            *count = written;
            return capacity < actualCount ? VK_INCOMPLETE : VK_SUCCESS;
        });

    CHECK(result.result == VK_SUCCESS);
    CHECK(result.attempts == 3);
    CHECK(result.values == std::vector<int>({1, 2, 3, 4, 5}));
}

void successfulShorterSnapshotShrinksTheResult()
{
    const auto result = enumerateVulkanValues<int>(
        [](std::uint32_t* count, int* values) {
            if (!values) {
                *count = 4;
                return VK_SUCCESS;
            }
            values[0] = 42;
            *count = 1;
            return VK_SUCCESS;
        });

    CHECK(result.result == VK_SUCCESS);
    CHECK(result.values == std::vector<int>({42}));
}

void errorsAreNotRetriedOrHidden()
{
    int calls = 0;
    const auto result = enumerateVulkanValues<int>(
        [&calls](std::uint32_t* count, int* values) {
            ++calls;
            if (!values) {
                *count = 1;
                return VK_SUCCESS;
            }
            return VK_ERROR_SURFACE_LOST_KHR;
        });

    CHECK(result.result == VK_ERROR_SURFACE_LOST_KHR);
    CHECK(result.attempts == 1);
    CHECK(result.values.empty());
    CHECK(calls == 2);
}

void persistentIncompleteIsBoundedAndReported()
{
    const auto result = enumerateVulkanValues<int>(
        [](std::uint32_t* count, int* values) {
            if (!values) {
                *count = 1;
                return VK_SUCCESS;
            }
            values[0] = 7;
            *count = 1;
            return VK_INCOMPLETE;
        }, 3);

    CHECK(result.result == VK_INCOMPLETE);
    CHECK(result.attempts == 3);
    CHECK(result.values.empty());
}

void voidPropertyEnumerationDetectsCountGrowth()
{
    int dataCalls = 0;
    const auto result = enumerateVulkanProperties<int>(
        [&dataCalls](std::uint32_t* count, int* values) {
            const auto available = dataCalls == 0 ? 2U : 3U;
            if (!values) {
                *count = available;
                return;
            }
            const auto written = std::min(*count, available);
            for (std::uint32_t index = 0; index < written; ++index) {
                values[index] = static_cast<int>(index + 1U);
            }
            *count = written;
            ++dataCalls;
        });

    CHECK(result.complete);
    CHECK(result.attempts == 2);
    CHECK(result.values == std::vector<int>({1, 2, 3}));
}

void voidPropertyEnumerationHasABoundedFailure()
{
    std::uint32_t available = 1;
    const auto result = enumerateVulkanProperties<int>(
        [&available](std::uint32_t* count, int* values) {
            if (!values) {
                *count = available;
                return;
            }
            const auto written = std::min(*count, available);
            std::fill_n(values, written, 1);
            *count = written;
            available = written + 1U;
        }, 3);

    CHECK(!result.complete);
    CHECK(result.attempts == 3);
    CHECK(result.values.empty());
}

} // namespace

int main()
{
    stableEnumerationCompletesInOneAttempt();
    incompleteResultRequeriesAndRecovers();
    geometricHeadroomEscapesARepeatedStaleCount();
    successfulShorterSnapshotShrinksTheResult();
    errorsAreNotRetriedOrHidden();
    persistentIncompleteIsBoundedAndReported();
    voidPropertyEnumerationDetectsCountGrowth();
    voidPropertyEnumerationHasABoundedFailure();

    if (failures != 0) {
        std::cerr << failures << " Vulkan enumeration check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Vulkan enumeration tests passed\n";
    return EXIT_SUCCESS;
}
