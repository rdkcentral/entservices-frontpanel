/**
* If not stated otherwise in this file or this component's LICENSE file the
* following copyright and licenses apply:
*
* Copyright 2026 RDK Management
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**/

#pragma once

// std inc
#include <gmock/gmock.h>

// interface inc
#include <interfaces/IDeviceSettingsFPD.h>

using ::testing::NiceMock;

namespace Core = WPEFramework::Core;
namespace Exchange = WPEFramework::Exchange;

/**
 * @brief gmock for Exchange::IDeviceSettingsFPD, the COM-RPC interface CFrontPanel
 *        acquires via CFrontPanel::setFPDAcquirer() (see helpers/frontpanel.cpp).
 *
 * Uses manual reference counting (not Core::ProxyType<T>) so lifetime is fully
 * deterministic: Get() hands out one ref owned by mockInstances(), Delete() drops
 * it, and the object deletes itself when the count reaches zero.
 */
class FrontPanelFPDMock : public Exchange::IDeviceSettingsFPD {

public:
    MOCK_METHOD(Core::hresult, Register, (const string clientName, Exchange::IDeviceSettingsFPD::INotification* notification), (override));
    MOCK_METHOD(Core::hresult, Unregister, (Exchange::IDeviceSettingsFPD::INotification* notification), (override));

    MOCK_METHOD(Core::hresult, SetFPDTime, (const FPDTimeFormat timeFormat, const uint32_t minutes, const uint32_t seconds), (override));
    MOCK_METHOD(Core::hresult, SetFPDScroll, (const uint32_t scrollHoldDuration, const uint32_t nHorizontalScrollIterations, const uint32_t nVerticalScrollIterations), (override));
    MOCK_METHOD(Core::hresult, SetFPDBlink, (const FPDIndicator indicator, const uint32_t blinkDuration, const uint32_t blinkIterations), (override));
    MOCK_METHOD(Core::hresult, SetFPDBrightness, (const FPDIndicator indicator, const uint32_t brightNess, const bool persist), (override));
    MOCK_METHOD(Core::hresult, GetFPDBrightness, (const FPDIndicator indicator, uint32_t& brightNess, const bool persist), (override));
    MOCK_METHOD(Core::hresult, SetFPDState, (const FPDIndicator indicator, const FPDState state), (override));
    MOCK_METHOD(Core::hresult, GetFPDState, (const FPDIndicator indicator, FPDState& state), (override));
    MOCK_METHOD(Core::hresult, GetFPDColor, (const FPDIndicator indicator, uint32_t& color), (override));
    MOCK_METHOD(Core::hresult, SetFPDColor, (const FPDIndicator indicator, const uint32_t color), (override));
    MOCK_METHOD(Core::hresult, SetFPDTextBrightness, (const FPDTextDisplay textDisplay, const uint32_t brightNess), (override));
    MOCK_METHOD(Core::hresult, GetFPDTextBrightness, (const FPDTextDisplay textDisplay, uint32_t& brightNess), (override));
    MOCK_METHOD(Core::hresult, EnableFPDClockDisplay, (const bool enable), (override));
    MOCK_METHOD(Core::hresult, GetFPDTimeFormat, (FPDTimeFormat& fpdTimeFormat), (override));
    MOCK_METHOD(Core::hresult, SetFPDTimeFormat, (const FPDTimeFormat fpdTimeFormat), (override));
    MOCK_METHOD(Core::hresult, SetFPDMode, (const FPDMode fpdMode), (override));

    uint32_t AddRef() const override
    {
        return ++_refCount;
    }

    uint32_t Release() const override
    {
        uint32_t result = --_refCount;
        if (result == 0) {
            delete this;
        }
        return result;
    }

    void* QueryInterface(const uint32_t interfaceNumber) override
    {
        if ((interfaceNumber == Core::IUnknown::ID) || (interfaceNumber == Exchange::IDeviceSettingsFPD::ID)) {
            AddRef();
            return static_cast<void*>(static_cast<Exchange::IDeviceSettingsFPD*>(this));
        }
        return nullptr;
    }

    static std::map<std::string, FrontPanelFPDMock*>& mockInstances()
    {
        static std::map<std::string, FrontPanelFPDMock*> mocks;
        return mocks;
    }

    static std::string testId()
    {
        auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        if (!testInfo) {
            return {};
        }
        return std::string(testInfo->test_suite_name()) + "#" + std::string(testInfo->name());
    }

    static Exchange::IDeviceSettingsFPD* Get()
    {
        std::string id = testId();
        ASSERT(!id.empty());

        auto& mocks = mockInstances();

        auto it = mocks.find(id);
        if (it == mocks.end()) {
            // Refcount starts at 1: this single ref is owned by mockInstances().
            mocks.insert(std::pair<std::string, FrontPanelFPDMock*>(id, new FrontPanelFPDMock()));
            it = mocks.find(id);
            ASSERT(it != mocks.end());
        }

        return it->second;
    }

    static FrontPanelFPDMock& Mock()
    {
        FrontPanelFPDMock* mock = static_cast<FrontPanelFPDMock*>(Get());
        return *mock;
    }

    static void Delete(void)
    {
        std::string id = testId();
        ASSERT_FALSE(id.empty()) << "testId should have been valid for all testcases";

        auto& mocks = mockInstances();

        auto it = mocks.find(id);
        if (it != mocks.end()) {
            // Drop mockInstances()' owning ref; deletes the object when it reaches zero.
            it->second->Release();
            mocks.erase(it);
        }
    }

private:
    mutable uint32_t _refCount = 1;
};
