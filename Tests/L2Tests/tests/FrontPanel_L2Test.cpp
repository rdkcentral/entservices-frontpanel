/* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2024 RDK Management
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

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "L2Tests.h"
#include "L2TestsMock.h"
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <interfaces/IFrontPanel.h>
#include <interfaces/IPowerManager.h>
#include "FrontPanelMock.h"
#include "PowerManagerMock.h"

#define JSON_TIMEOUT   (1000)
#define TEST_LOG(x, ...) fprintf(stderr, "\033[1;32m[%s:%d](%s)<PID:%d><TID:%d>" x "\n\033[0m", __FILE__, __LINE__, __FUNCTION__, getpid(), gettid(), ##__VA_ARGS__); fflush(stderr);
#define FRONTPANEL_CALLSIGN  _T("org.rdk.FrontPanel.1")
#define L2TEST_CALLSIGN _T("L2tests.1")

using ::testing::NiceMock;
using namespace WPEFramework;
using testing::StrictMock;
using PowerState = WPEFramework::Exchange::IPowerManager::PowerState;

typedef enum : uint32_t {
    FRONTPANELL2TEST_BRIGHTNESS_CHANGED = 0x00000001,
    FRONTPANELL2TEST_LED_STATE_CHANGED = 0x00000002,
    FRONTPANELL2TEST_BLINK_COMPLETED = 0x00000004,
    FRONTPANELL2TEST_POWER_STATE_CHANGED = 0x00000008,
    FRONTPANELL2TEST_STATE_INVALID = 0x00000000
} FrontPanelL2test_async_events_t;

class FrontPanel_Notification : public Exchange::IPowerManager::IModeChangedNotification {
    private:
        std::mutex m_mutex;
        std::condition_variable m_condition_variable;
        uint32_t m_event_signalled;

        BEGIN_INTERFACE_MAP(FrontPanel_Notification)
        INTERFACE_ENTRY(Exchange::IPowerManager::IModeChangedNotification)
        END_INTERFACE_MAP

    public:
        FrontPanel_Notification() : m_event_signalled(FRONTPANELL2TEST_STATE_INVALID) {}
        ~FrontPanel_Notification() {}

        void OnPowerModeChanged(const PowerState currentState, const PowerState newState) override {
            TEST_LOG("OnPowerModeChanged event triggered ***\n");
            std::unique_lock<std::mutex> lock(m_mutex);
            TEST_LOG("OnPowerModeChanged currentState: %u, newState: %u\n", currentState, newState);
            m_event_signalled |= FRONTPANELL2TEST_POWER_STATE_CHANGED;
            m_condition_variable.notify_one();
        }

        uint32_t WaitForRequestStatus(uint32_t timeout_ms, FrontPanelL2test_async_events_t expected_status) {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto now = std::chrono::system_clock::now();
            std::chrono::milliseconds timeout(timeout_ms);

            while (!(expected_status & m_event_signalled)) {
                if (m_condition_variable.wait_until(lock, now + timeout) == std::cv_status::timeout) {
                    return FRONTPANELL2TEST_STATE_INVALID;
                }
            }

            uint32_t signalled = m_event_signalled;
            m_event_signalled = FRONTPANELL2TEST_STATE_INVALID;
            return signalled;
        }
};

/* FrontPanel L2 test class declaration */
class FrontPanel_L2Test : public L2TestMocks {
protected:
    PluginHost::IShell* m_controller_FrontPanel;
    Exchange::IFrontPanel* m_frontPanelPlugin;

public:
    FrontPanel_L2Test();
    virtual ~FrontPanel_L2Test() override;

    uint32_t CreateDeviceFrontPanelInterfaceObject();

    // void Test_SetGetBrightness(Exchange::IFrontPanel* FrontPanelPlugin);
    // void Test_PowerLedOnOff(Exchange::IFrontPanel* FrontPanelPlugin);
    // void Test_GetFrontPanelLights(Exchange::IFrontPanel* FrontPanelPlugin);
    // void Test_SetLED(Exchange::IFrontPanel* FrontPanelPlugin);
    // void Test_SetBlink(Exchange::IFrontPanel* FrontPanelPlugin);
    // void Test_PowerStateIntegration(Exchange::IFrontPanel* FrontPanelPlugin, Exchange::IPowerManager* PowerManagerPlugin);

    Core::Sink<FrontPanel_Notification> mNotification;

private:
    std::mutex m_mutex;
    std::condition_variable m_condition_variable;
    uint32_t m_event_signalled;
};

/**
 * @brief Constructor for FrontPanel L2 test class
 */
FrontPanel_L2Test::FrontPanel_L2Test() 
    : L2TestMocks()
    , m_controller_FrontPanel(nullptr)
    , m_frontPanelPlugin(nullptr)
    , m_event_signalled(FRONTPANELL2TEST_STATE_INVALID) {

    uint32_t status = Core::ERROR_GENERAL;

    // Activate the actual plugin service
    status = ActivateService("org.rdk.FrontPanel");
    EXPECT_EQ(Core::ERROR_NONE, status);
}

/**
 * @brief Destructor for FrontPanel L2 test class
 */
FrontPanel_L2Test::~FrontPanel_L2Test() {
    uint32_t status = Core::ERROR_GENERAL;

    if (m_frontPanelPlugin) {
        m_frontPanelPlugin->Release();
        m_frontPanelPlugin = nullptr;
    }

    if (m_controller_FrontPanel) {
        m_controller_FrontPanel->Release();
        m_controller_FrontPanel = nullptr;
    }

    status = DeactivateService("org.rdk.FrontPanel");
    EXPECT_EQ(Core::ERROR_NONE, status);
}

uint32_t FrontPanel_L2Test::CreateDeviceFrontPanelInterfaceObject() {
    uint32_t return_value = Core::ERROR_GENERAL;
    Core::ProxyType<RPC::InvokeServerType<1, 0, 4>> FrontPanel_Engine;
    Core::ProxyType<RPC::CommunicatorClient> FrontPanel_Client;

    TEST_LOG("Creating FrontPanel_Engine");
    FrontPanel_Engine = Core::ProxyType<RPC::InvokeServerType<1, 0, 4>>::Create();
    FrontPanel_Client = Core::ProxyType<RPC::CommunicatorClient>::Create(Core::NodeId("/tmp/communicator"), Core::ProxyType<Core::IIPCServer>(FrontPanel_Engine));

    TEST_LOG("Creating FrontPanel_Engine Announcements");
#if ((THUNDER_VERSION == 2) || ((THUNDER_VERSION == 4) && (THUNDER_VERSION_MINOR == 2)))
    FrontPanel_Engine->Announcements(FrontPanel_Client->Announcement());
#endif
    if (!FrontPanel_Client.IsValid()) {
        TEST_LOG("Invalid FrontPanel_Client");
    } else {
        m_controller_FrontPanel = FrontPanel_Client->Open<PluginHost::IShell>(_T("org.rdk.FrontPanel"), ~0, 3000);
        if (m_controller_FrontPanel) {
            m_frontPanelPlugin = m_controller_FrontPanel->QueryInterface<Exchange::IFrontPanel>();
            return_value = Core::ERROR_NONE;
        }
    }
    return return_value;
}

TEST_F(FrontPanel_L2Test, Test_SetGetBrightness)
{
    printf("**** Starting Test_SetGetBrightness ****\n");
}
