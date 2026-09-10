/**
* If not stated otherwise in this file or this component's LICENSE
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

#include "FrontPanel.h"
#include "FrontPanelImplementation.h"
#include "frontpanel.h"
#include "frontpanel.cpp"
#include "FrontPanelMock.h"
#include "FrontPanelFPDMock.h"
#include "WorkerPoolImplementation.h"
#include "WrapsMock.h"
#include "COMLinkMock.h"

#include "FactoriesImplementation.h"

#include "ServiceMock.h"
#include "PowerManagerMock.h"
#include "ThunderPortability.h"

// Previously pulled in transitively via IarmBusMock.h (no longer included).
#ifndef TEST_LOG
#define TEST_LOG(x, ...) fprintf(stderr, "\033[1;32m[%s:%d](%s)<PID:%d><TID:%d>" x "\n\033[0m", __FILE__, __LINE__, __FUNCTION__, getpid(), gettid(), ##__VA_ARGS__); fflush(stderr);
#endif

using namespace WPEFramework;
using IPowerManager = Exchange::IPowerManager;
using FPD = Exchange::IDeviceSettingsFPD;

using testing::Eq;
using testing::NiceMock;

class FrontPanelTest : public ::testing::Test {
protected:
    Core::ProxyType<Plugin::FrontPanel> plugin;
    Core::JSONRPC::Handler& handler;
    DECL_CORE_JSONRPC_CONX connection;
    NiceMock<ServiceMock> service;
    NiceMock<COMLinkMock> comLinkMock;
    Core::ProxyType<WorkerPoolImplementation> workerPool;
    Core::ProxyType<Plugin::FrontPanelImplementation> FrontPanelImplem;
    NiceMock<FactoriesImplementation> factoriesImplementation;
    PLUGINHOST_DISPATCHER *dispatcher;
    string response;
    Core::JSONRPC::Message message;
    ServiceMock  *p_serviceMock  = nullptr;
    WrapsImplMock* p_wrapsImplMock = nullptr;
    FrontPanelMock* p_frontPanelMock = nullptr;

    FrontPanelTest()
        : plugin(Core::ProxyType<Plugin::FrontPanel>::Create())
        , handler(*plugin)
        , connection(0,1,"")
        , workerPool(Core::ProxyType<WorkerPoolImplementation>::Create(
            2, Core::Thread::DefaultStackSize(), 16))
    {

        p_serviceMock = new NiceMock <ServiceMock>;

        p_frontPanelMock  = new NiceMock <FrontPanelMock>;

        p_wrapsImplMock = new NiceMock<WrapsImplMock>;
        Wraps::setImpl(p_wrapsImplMock);

        PluginHost::IFactories::Assign(&factoriesImplementation);

        dispatcher = static_cast<PLUGINHOST_DISPATCHER*>(
        plugin->QueryInterface(PLUGINHOST_DISPATCHER_ID));
        dispatcher->Activate(&service);


        ON_CALL(service, COMLink())
            .WillByDefault(::testing::Invoke(
                  [this]() {
                        TEST_LOG("Pass created comLinkMock: %p ", &comLinkMock);
                        return &comLinkMock;
                    }));

        ON_CALL(comLinkMock, Instantiate(::testing::_, ::testing::_, ::testing::_))
                .WillByDefault(::testing::Invoke(
                    [&](const RPC::Object& object, const uint32_t waitTime, uint32_t& connectionId) {
                        FrontPanelImplem = Core::ProxyType<Plugin::FrontPanelImplementation>::Create();
                        return &FrontPanelImplem;
                    }));

        Core::IWorkerPool::Assign(&(*workerPool));
            workerPool->Run();

    }

    virtual ~FrontPanelTest()
    {
        // Ensure we deactivate dispatcher before releasing it
        if (dispatcher != nullptr) {
            // Deactivate if it was activated (safe even if not)
            dispatcher->Deactivate();
            dispatcher->Release();
            dispatcher = nullptr;
        }

        // Restore global factory hooks
        PluginHost::IFactories::Assign(nullptr);

        // Clear Wraps implementation and delete allocated mocks
        Wraps::setImpl(nullptr);
        delete p_wrapsImplMock;
        p_wrapsImplMock = nullptr;

        delete p_frontPanelMock;
        p_frontPanelMock = nullptr;

        delete p_serviceMock;
        p_serviceMock = nullptr;

    }
};

/**
 * FrontPanelInitializedTest activates the plugin (which opens the COM-RPC link
 * to DeviceSettings via DSHelper::Open()) and then, mirroring what
 * FrontPanelImplementation::OnDeviceSettingsActivated() would do once the real
 * DeviceSettings plugin is up, injects a mock IDeviceSettingsFPD directly via
 * CFrontPanel::setFPDAcquirer(). This sidesteps the need to fake Thunder's
 * internal plugin-monitor/QueryInterfaceByCallsign machinery - CFrontPanel is
 * agnostic to how its acquirer resolves the interface.
 */
class FrontPanelInitializedTest : public FrontPanelTest {
protected:
    FrontPanelFPDMock* p_fpdMock = nullptr;
    IPowerManager::IModeChangedNotification* _notification = nullptr;

    FrontPanelInitializedTest()
        : FrontPanelTest()
    {
        ON_CALL(service, QueryInterfaceByCallsign(::testing::_, ::testing::StrEq("org.rdk.PowerManager")))
            .WillByDefault(::testing::Invoke(
                [&](const uint32_t interfaceId, const string& name) -> void* {
                    return PowerManagerMock::Get();
                }));

        EXPECT_CALL(PowerManagerMock::Mock(), Register(::testing::Matcher<Exchange::IPowerManager::IModeChangedNotification*>(::testing::_)))
            .WillOnce(
                [this](IPowerManager::IModeChangedNotification* notification) -> uint32_t {
                    _notification = notification;
                    return Core::ERROR_NONE;
                });

        EXPECT_EQ(string(""), plugin->Initialize(&service));

        p_fpdMock = static_cast<FrontPanelFPDMock*>(FrontPanelFPDMock::Get());
        Plugin::CFrontPanel::instance()->setFPDAcquirer([&]() {
            p_fpdMock->AddRef();
            return static_cast<FPD*>(p_fpdMock);
        });
    }
    virtual ~FrontPanelInitializedTest() override
    {
        Plugin::CFrontPanel::instance()->clearFPDInterface();

        plugin->Deinitialize(&service);

        // FrontPanelImplem (a FrontPanelTest member) outlives this destructor otherwise,
        // keeping FrontPanelImplementation - and its _powerManagerPlugin ref on
        // PowerManagerMock - alive past Delete(), which is what was leaking the mock.
        FrontPanelImplem = Core::ProxyType<Plugin::FrontPanelImplementation>();

        _notification = nullptr;
        PowerManagerMock::Delete();
        FrontPanelFPDMock::Delete();
        p_fpdMock = nullptr;

        // Clearing out out-of-scope state, and resetting initDone to 0.
        Plugin::CFrontPanel::initDone = 0;
    }
};

class FrontPanelInitializedEventTest : public FrontPanelInitializedTest {
protected:
    testing::NiceMock<ServiceMock> service;
    FactoriesImplementation factoriesImplementation;
    PLUGINHOST_DISPATCHER* dispatcher;
    Core::JSONRPC::Message message;

    FrontPanelInitializedEventTest()
        : FrontPanelInitializedTest()
    {
        PluginHost::IFactories::Assign(&factoriesImplementation);

        dispatcher = static_cast<PLUGINHOST_DISPATCHER*>(
            plugin->QueryInterface(PLUGINHOST_DISPATCHER_ID));

        dispatcher->Activate(&service);
    }

    virtual ~FrontPanelInitializedEventTest() override
    {
        dispatcher->Deactivate();
        dispatcher->Release();

        PluginHost::IFactories::Assign(nullptr);
    }
};

/**
 * FrontPanelInitializedEventDsTest additionally simulates a PowerManager
 * "STANDBY -> ON" transition, which is what the real system does before the
 * front panel becomes interactive.
 */
class FrontPanelInitializedEventDsTest : public FrontPanelInitializedEventTest {
protected:

    FrontPanelInitializedEventDsTest()
        : FrontPanelInitializedEventTest()
    {
        EXPECT_NE(_notification, nullptr);
        _notification->OnPowerModeChanged(IPowerManager::POWER_STATE_STANDBY, IPowerManager::POWER_STATE_ON);
    }
};

TEST_F(FrontPanelInitializedTest, RegisteredMethods)
{
    EXPECT_EQ(Core::ERROR_NONE, handler.Exists(_T("setBrightness")));
    EXPECT_EQ(Core::ERROR_NONE, handler.Exists(_T("getBrightness")));
    EXPECT_EQ(Core::ERROR_NONE, handler.Exists(_T("powerLedOn")));
    EXPECT_EQ(Core::ERROR_NONE, handler.Exists(_T("powerLedOff")));
    EXPECT_EQ(Core::ERROR_NONE, handler.Exists(_T("getFrontPanelLights")));
    EXPECT_EQ(Core::ERROR_NONE, handler.Exists(_T("setLED")));
    EXPECT_EQ(Core::ERROR_NONE, handler.Exists(_T("setBlink")));
}

TEST_F(FrontPanelInitializedEventDsTest, setBrightnessWIndex)
{
    EXPECT_CALL(*p_fpdMock, SetFPDBrightness(FPD::DS_FPD_INDICATOR_POWER, 1, true))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("setBrightness"), _T("{\"brightness\": 1,\"index\": \"power_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, setBrightness)
{
    // No "index" supplied: CFrontPanel::setBrightness() sweeps every FPDIndicator.
    for (uint8_t i = 0; i < static_cast<uint8_t>(FPD::DS_FPD_INDICATOR_MAX); ++i) {
        EXPECT_CALL(*p_fpdMock, SetFPDBrightness(static_cast<FPD::FPDIndicator>(i), 1, true))
            .Times(1)
            .WillOnce(::testing::Return(Core::ERROR_NONE));
    }

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("setBrightness"), _T("{\"brightness\": 1}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, setBrightnessFPDError)
{
    EXPECT_CALL(*p_fpdMock, SetFPDBrightness(FPD::DS_FPD_INDICATOR_POWER, 1, true))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_GENERAL));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("setBrightness"), _T("{\"brightness\": 1,\"index\": \"power_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":false}"));
}

TEST_F(FrontPanelInitializedEventDsTest, setBrightnessFPDUnavailable)
{
    // Simulate DeviceSettings being unreachable: clear the acquirer entirely.
    Plugin::CFrontPanel::instance()->clearFPDInterface();

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("setBrightness"), _T("{\"brightness\": 1,\"index\": \"power_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":false}"));
}

TEST_F(FrontPanelInitializedEventDsTest, getBrightnessWIndex)
{
    EXPECT_CALL(*p_fpdMock, GetFPDBrightness(FPD::DS_FPD_INDICATOR_POWER, ::testing::_, false))
        .Times(1)
        .WillOnce(::testing::Invoke(
            [](FPD::FPDIndicator, uint32_t& brightNess, bool) {
                brightNess = 50;
                return Core::ERROR_NONE;
            }));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("getBrightness"), _T("{\"index\": \"power_led\"}"), response));
    EXPECT_EQ(response, string("{\"brightness\":50,\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, getBrightnessNumericIndex)
{
    // Unrecognised name that parses as a numeric FPDIndicator index (4 = RFBYPASS).
    EXPECT_CALL(*p_fpdMock, GetFPDBrightness(FPD::DS_FPD_INDICATOR_RFBYPASS, ::testing::_, false))
        .Times(1)
        .WillOnce(::testing::Invoke(
            [](FPD::FPDIndicator, uint32_t& brightNess, bool) {
                brightNess = 42;
                return Core::ERROR_NONE;
            }));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("getBrightness"), _T("{\"index\": \"4\"}"), response));
    EXPECT_EQ(response, string("{\"brightness\":42,\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, getBrightness)
{
    EXPECT_CALL(*p_fpdMock, GetFPDBrightness(FPD::DS_FPD_INDICATOR_POWER, ::testing::_, false))
        .Times(1)
        .WillOnce(::testing::Invoke(
            [](FPD::FPDIndicator, uint32_t& brightNess, bool) {
                brightNess = 50;
                return Core::ERROR_NONE;
            }));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("getBrightness"), _T(""), response));
    EXPECT_EQ(response, string("{\"brightness\":50,\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, getFrontPanelLights)
{
    // Without a live DeviceSettings COM-RPC config, DSHelper::getFPDIndicators()/
    // getFPDColors()/getFPDColorBindings() are empty; GetFrontPanelLights still
    // reports success=true with an empty light list/info payload.
    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("getFrontPanelLights"), _T(""), response));
    EXPECT_TRUE(response.find("\"success\":true") != std::string::npos);
}

TEST_F(FrontPanelInitializedEventDsTest, powerLedOffPower)
{
    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_POWER, FPD::DS_FPD_STATE_OFF))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("powerLedOff"), _T("{\"index\": \"power_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, powerLedOffData)
{
    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_MESSAGE, FPD::DS_FPD_STATE_OFF))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("powerLedOff"), _T("{\"index\": \"data_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, powerLedOffRecord)
{
    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_RECORD, FPD::DS_FPD_STATE_OFF))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("powerLedOff"), _T("{\"index\": \"record_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, powerLedOnPower)
{
    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_POWER, FPD::DS_FPD_STATE_ON))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("powerLedOn"), _T("{\"index\": \"power_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));

    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_RECORD, FPD::DS_FPD_STATE_ON))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("powerLedOn"), _T("{\"index\": \"record_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));

    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_MESSAGE, FPD::DS_FPD_STATE_ON))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("powerLedOn"), _T("{\"index\": \"data_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, powerLedOnFPDUnavailable)
{
    Plugin::CFrontPanel::instance()->clearFPDInterface();

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("powerLedOn"), _T("{\"index\": \"power_led\"}"), response));
    EXPECT_EQ(response, string("{\"success\":false}"));
}

TEST_F(FrontPanelInitializedEventDsTest, setBlink)
{
    // pattern[0]: brightness=50 (used as-is), red/green/blue=2/2/2 -> colorValue 0x020202 (131586).
    EXPECT_CALL(*p_fpdMock, SetFPDColor(FPD::DS_FPD_INDICATOR_POWER, 131586u))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_CALL(*p_fpdMock, SetFPDBrightness(FPD::DS_FPD_INDICATOR_POWER, 50, false))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("setBlink"), _T("{\"blinkInfo\": {\"ledIndicator\": \"power_led\", \"iterations\": 10, \"pattern\": [{\"brightness\": 50, \"duration\": 1000, \"red\": 2, \"green\":2, \"blue\":2}]}}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, setLEDMode1)
{
    // RGB path: red=green=blue=0 -> colorValue 0.
    EXPECT_CALL(*p_fpdMock, SetFPDColor(FPD::DS_FPD_INDICATOR_POWER, 0u))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_CALL(*p_fpdMock, SetFPDBrightness(FPD::DS_FPD_INDICATOR_POWER, 50, false))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("setLED"), _T("{\"ledIndicator\": \"power_led\", \"brightness\": 50, \"red\": 0, \"green\": 0, \"blue\":0}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, setLEDMode2)
{
    // Named-color path: "red" -> 0xFF0000.
    EXPECT_CALL(*p_fpdMock, SetFPDColor(FPD::DS_FPD_INDICATOR_POWER, 0xFF0000u))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_CALL(*p_fpdMock, SetFPDBrightness(FPD::DS_FPD_INDICATOR_POWER, 50, false))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));

    EXPECT_EQ(Core::ERROR_NONE, handler.Invoke(connection, _T("setLED"), _T("{\"ledIndicator\": \"power_led\", \"brightness\": 50, \"color\": \"red\", \"red\": 1, \"green\": 2, \"blue\":3}"), response));
    EXPECT_EQ(response, string("{\"success\":true}"));
}

TEST_F(FrontPanelInitializedEventDsTest, setLEDUnsupportedColor)
{
    // Unrecognised color name (not a known name or #RRGGBB literal) fails setLED.
    // Unlike SetBrightness/SetBlink, SetLED's hresult reflects failure (ERROR_GENERAL),
    // so the JSON-RPC layer never serializes a "{"success":false}" body.
    EXPECT_EQ(Core::ERROR_GENERAL, handler.Invoke(connection, _T("setLED"), _T("{\"ledIndicator\": \"power_led\", \"brightness\": 50, \"color\": \"purple\"}"), response));
}

// --- Negative / direct CFrontPanel test cases ---

TEST_F(FrontPanelInitializedEventDsTest, powerLedOffExtended)
{
    // Get the singleton instance to test its methods directly
    Plugin::CFrontPanel* frontPanel = Plugin::CFrontPanel::instance();
    ASSERT_NE(frontPanel, nullptr);

    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_REMOTE, FPD::DS_FPD_STATE_OFF))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_TRUE(frontPanel->powerOffLed(Plugin::FRONT_PANEL_INDICATOR_REMOTE));

    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_RFBYPASS, FPD::DS_FPD_STATE_OFF))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_TRUE(frontPanel->powerOffLed(Plugin::FRONT_PANEL_INDICATOR_RFBYPASS));

    for (uint8_t i = 0; i < static_cast<uint8_t>(FPD::DS_FPD_INDICATOR_MAX); ++i) {
        EXPECT_CALL(*p_fpdMock, SetFPDState(static_cast<FPD::FPDIndicator>(i), FPD::DS_FPD_STATE_OFF))
            .Times(1)
            .WillOnce(::testing::Return(Core::ERROR_NONE));
    }
    EXPECT_TRUE(frontPanel->powerOffAllLed());
}

TEST_F(FrontPanelInitializedEventDsTest, powerLedOnExtended)
{
    Plugin::CFrontPanel* frontPanel = Plugin::CFrontPanel::instance();
    ASSERT_NE(frontPanel, nullptr);

    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_REMOTE, FPD::DS_FPD_STATE_ON))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_TRUE(frontPanel->powerOnLed(Plugin::FRONT_PANEL_INDICATOR_REMOTE));

    EXPECT_CALL(*p_fpdMock, SetFPDState(FPD::DS_FPD_INDICATOR_RFBYPASS, FPD::DS_FPD_STATE_ON))
        .Times(1)
        .WillOnce(::testing::Return(Core::ERROR_NONE));
    EXPECT_TRUE(frontPanel->powerOnLed(Plugin::FRONT_PANEL_INDICATOR_RFBYPASS));

    for (uint8_t i = 0; i < static_cast<uint8_t>(FPD::DS_FPD_INDICATOR_MAX); ++i) {
        EXPECT_CALL(*p_fpdMock, SetFPDState(static_cast<FPD::FPDIndicator>(i), FPD::DS_FPD_STATE_ON))
            .Times(1)
            .WillOnce(::testing::Return(Core::ERROR_NONE));
    }
    EXPECT_TRUE(frontPanel->powerOnAllLed());
}
