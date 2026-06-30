/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2019 RDK Management
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

#include "FrontPanelImplementation.h"
#include "frontpanel.h"
#include <algorithm>

#ifndef USE_DEVICESETTING_PLUGIN
#include "frontPanelIndicator.hpp"
#include "frontPanelConfig.hpp"
#include "frontPanelTextDisplay.hpp"

#include "libIBus.h"
#endif

#include "UtilsJsonRpc.h"
#include "UtilsIarm.h"

#define SERVICE_NAME "FrontPanelService"
#define METHOD_FP_SET_BRIGHTNESS "setBrightness"
#define METHOD_FP_GET_BRIGHTNESS "getBrightness"
#define METHOD_FP_POWER_LED_ON "powerLedOn"
#define METHOD_FP_POWER_LED_OFF "powerLedOff"
#define METHOD_GET_FRONT_PANEL_LIGHTS "getFrontPanelLights"
#define METHOD_FP_SET_LED "setLED"
#define METHOD_FP_SET_BLINK "setBlink"

#define DATA_LED "data_led"
#define RECORD_LED "record_led"
#define POWER_LED "power_led"

#ifdef USE_EXTENDED_ALL_SEGMENTS_TEXT_PATTERN
#define ALL_SEGMENTS_TEXT_PATTERN "88:88"
#else
#define ALL_SEGMENTS_TEXT_PATTERN "8888"
#endif

#define DEFAULT_TEXT_PATTERN_UPDATE_INTERVAL 5

#define API_VERSION_NUMBER_MAJOR 1
#define API_VERSION_NUMBER_MINOR 0
#define API_VERSION_NUMBER_PATCH 6

using PowerState = WPEFramework::Exchange::IPowerManager::PowerState;


namespace
{

    struct Mapping
    {
        const char *IArmBusName;
        const char *SvcManagerName;
    };

    static struct Mapping name_mappings[] = {
        { "Record" , "record_led"},
        { "Message" , "data_led"},
        { "Power" , "power_led"},
        // TODO: add your mappings here
        // { <IARM_NAME>, <SVC_MANAGER_API_NAME> },
        { 0,  0}
    };

    string svc2iarm(const string &name)
    {
        const char *s = name.c_str();

        int i = 0;
        while (name_mappings[i].SvcManagerName)
        {
            if (strcmp(s, name_mappings[i].SvcManagerName) == 0)
                return name_mappings[i].IArmBusName;
            i++;
        }
        return name;
    }


}

namespace WPEFramework
{

    namespace Plugin
    {
        SERVICE_REGISTRATION(FrontPanelImplementation, API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH);

        FrontPanelImplementation* FrontPanelImplementation::_instance = nullptr;

        FrontPanelImplementation::FrontPanelImplementation()
        : m_runUpdateTimer(false)
        , _pwrMgrNotification(*this)
        , _registeredEventHandlers(false)
#ifdef USE_DEVICESETTING_PLUGIN
        , _dsFpdNotification(*this)
#endif
        {
            FrontPanelImplementation::_instance = this;
            m_runUpdateTimer = false;

        }

        FrontPanelImplementation::~FrontPanelImplementation()
        {
            if (_powerManagerPlugin) {
                _powerManagerPlugin->Unregister(_pwrMgrNotification.baseInterface<Exchange::IPowerManager::IModeChangedNotification>());
                _powerManagerPlugin.Reset();
            }

#ifdef USE_DEVICESETTING_PLUGIN
            {
                auto* fpd = AcquireSubInterface<Exchange::IDeviceSettingsFPD>();
                if (fpd) {
                    fpd->Unregister(&_dsFpdNotification);
                    fpd->Release();
                }
            }
            DeviceSettingsClientHelper::Close();
#else
            CFrontPanel::instance()->deinitialize();
#endif
            _registeredEventHandlers = false;
            FrontPanelImplementation::_instance = nullptr;
        }

        Core::hresult FrontPanelImplementation::Configure(PluginHost::IShell* service)
        {
            InitializePowerManager(service);
            FrontPanelImplementation::_instance = this;
#ifdef USE_DEVICESETTING_PLUGIN
            DeviceSettingsClientHelper::Open(service);
#else
            CFrontPanel::instance(service);
            CFrontPanel::instance()->start();
            CFrontPanel::instance()->addEventObserver(this);
#endif
            return Core::ERROR_NONE;
        }

        void FrontPanelImplementation::InitializePowerManager(PluginHost::IShell *service)
        {
            _powerManagerPlugin = PowerManagerInterfaceBuilder(_T("org.rdk.PowerManager"))
                                        .withIShell(service)
                                        .withRetryIntervalMS(200)
                                        .withRetryCount(25)
                                        .createInterface();
            registerEventHandlers();
        }

        void FrontPanelImplementation::onPowerModeChanged(const PowerState currentState, const PowerState newState)
        {
#ifdef USE_DEVICESETTING_PLUGIN
            // In the COM-RPC path the DeviceSettings plugin manages front panel power state
            // internally; no explicit CFrontPanel gate is needed here.
            LOGINFO("onPowerModeChanged: newState=%d (DS plugin path — no CFrontPanel call)", static_cast<int>(newState));
#else
            if(newState == WPEFramework::Exchange::IPowerManager::POWER_STATE_ON)
            {
                LOGINFO("setPowerStatus true");
                CFrontPanel::instance()->setPowerStatus(true);
            }
            else
            {
                LOGINFO("setPowerStatus false");
                CFrontPanel::instance()->setPowerStatus(false);
            }
#endif
            return;
        }

        void FrontPanelImplementation::registerEventHandlers()
        {
            ASSERT (_powerManagerPlugin);

            if(!_registeredEventHandlers && _powerManagerPlugin) {
                _registeredEventHandlers = true;
                _powerManagerPlugin->Register(_pwrMgrNotification.baseInterface<Exchange::IPowerManager::IModeChangedNotification>());
            }
        }

        Core::hresult FrontPanelImplementation::SetBrightness(const string& index, const uint32_t brightness, FrontPanelSuccess& success)
        {
            LOGINFO("SetBrightness called with index: %s, brightness: %d", index.c_str(), brightness);
            bool ok = false;

            string fp_ind = svc2iarm(index);
            if (!fp_ind.empty())
            {
                // Per-indicator brightness — delegated to CFrontPanel (handles DS/libds internally)
                ok = CFrontPanel::instance()->setBrightnessByName(fp_ind, static_cast<int>(brightness));
            }
            else if (brightness <= 100)
            {
                // Global brightness — delegated to CFrontPanel
                ok = CFrontPanel::instance()->setBrightness(static_cast<int>(brightness));
            }
            else
            {
                LOGWARN("Invalid brightnessLevel passed to SetBrightness");
            }

            success.success = ok;
            return Core::ERROR_NONE;
        }

        /**
         * @brief Gets the brightness of the specified LED.
         *
         * @param[in] argList List of arguments (Not used).
         *
         * @return Returns a ServiceParams object containing brightness value and function result.
         * @ingroup SERVMGR_FRONTPANEL_API
         */
        Core::hresult FrontPanelImplementation::GetBrightness(const string& index, uint32_t& brightness, bool& success)
        {
            LOGINFO("GetBrightness called with index: %s", index.c_str());
            bool ok = false;
            int value = -1;

            string fp_ind = svc2iarm(index);
            if (!fp_ind.empty())
            {
                // Per-indicator — delegated to CFrontPanel (handles DS/libds internally)
                value = CFrontPanel::instance()->getBrightnessByName(fp_ind);
            }
            else
            {
                // Global — delegated to CFrontPanel
                value = CFrontPanel::instance()->getBrightness();
            }

            if (value >= 0)
            {
                brightness = static_cast<uint32_t>(value);
                ok = true;
            }
            else
            {
                brightness = static_cast<uint32_t>(-1);
                ok = false;
            }

            success = ok;
            return Core::ERROR_NONE;
        }

        Core::hresult FrontPanelImplementation::PowerLedOn(const string& index, FrontPanelSuccess& success)
        {
            bool ok = false;
            if (index == DATA_LED) {
                ok = CFrontPanel::instance()->powerOnLed(FRONT_PANEL_INDICATOR_MESSAGE);
            } else if (index == RECORD_LED) {
                ok = CFrontPanel::instance()->powerOnLed(FRONT_PANEL_INDICATOR_RECORD);
            } else if (index == POWER_LED) {
                ok = CFrontPanel::instance()->powerOnLed(FRONT_PANEL_INDICATOR_POWER);
            }
            success.success = ok;
            return Core::ERROR_NONE;
        }

        Core::hresult FrontPanelImplementation::PowerLedOff(const string& index, FrontPanelSuccess& success)
        {
            bool ok = false;
            if (index == DATA_LED) {
                ok = CFrontPanel::instance()->powerOffLed(FRONT_PANEL_INDICATOR_MESSAGE);
            } else if (index == RECORD_LED) {
                ok = CFrontPanel::instance()->powerOffLed(FRONT_PANEL_INDICATOR_RECORD);
            } else if (index == POWER_LED) {
                ok = CFrontPanel::instance()->powerOffLed(FRONT_PANEL_INDICATOR_POWER);
            }
            success.success = ok;
            return Core::ERROR_NONE;
        }

        /**
         * @brief getFrontPanelLights This returns an object containing attributes of front panel
         * light: success, supportedLights, and supportedLightsInfo.
         * supportedLights defines the LED lights that can be controlled through the Front Panel API.
         * supportedLightsInfo defines a hash of objects describing each LED light.
         * success - false if the supported lights info was unable to be determined.
         *
         * @return Returns a list of front panel lights parameter.
         * @ingroup SERVMGR_FRONTPANEL_API
         */
        std::vector<std::string> FrontPanelImplementation::getFrontPanelLights()
        {
            // All DS/libds logic lives in CFrontPanel
            return CFrontPanel::instance()->getFrontPanelLights();
        }

        /**
         * @brief getFrontPanelLightsInfo This returns an object containing attributes of front
         * panel light: success, supportedLights, and supportedLightsInfo.
         * supportedLightsInfo defines a hash of objects describing each LED light properties such as
         * -"range" Determines the types of values that can be expected in min and max value.
         * -"min" The minimum value is equivalent to off i.e "0".
         * -"max" The maximum value is when the LED is on i.e "1" and at its brightest.
         * -"step" The step or interval between the min and max values supported by the LED.
         * -"colorMode" Defines enum of "0" LED's color cannot be changed, "1"  LED can be set to any color
         * (using rgb-hex code),"2"  LED can be set to an enumeration of colors as specified by the
         * supportedColors property.
         *
         * @return Returns a serviceParams list of front panel lights info.
         */

        JsonObject FrontPanelImplementation::getFrontPanelLightsInfo()
        {
            // All DS/libds logic lives in CFrontPanel
            return CFrontPanel::instance()->getFrontPanelLightsInfo();
        }

        Core::hresult FrontPanelImplementation::GetFrontPanelLights(IFrontPanelLightsListIterator*& supportedLights , string &supportedLightsInfo, bool &success)
        {
            LOGINFO("[%s][%d]GetFrontPanelLights called", __FUNCTION__, __LINE__);
            std::vector<std::string> frontPanelLights;
            frontPanelLights = getFrontPanelLights();
            
            JsonObject info = getFrontPanelLightsInfo();
            string infoStr;
            info.ToString(infoStr);
            supportedLightsInfo = std::move(infoStr);
            success = true;

            supportedLights = (Core::Service<RPC::IteratorType<Exchange::IFrontPanel::IFrontPanelLightsListIterator>>::Create<Exchange::IFrontPanel::IFrontPanelLightsListIterator>(frontPanelLights));
            return Core::ERROR_NONE;
        }

        /**
         * @brief Sets the brightness and color properties of the specified LED.
         * The supported properties of the info object passed in will be determined by the color
         * mode of the LED. If the colorMode of an LED is 0 color values will be ignored. If the
         * brightness of the LED is unspecified or value = -1, then the persisted or default
         * value for the system is used.
         *
         * @param[in] properties Key value pair of properties data.
         *
         * @return Returns success value of the helper method, returns false in case of failure.
         */
        Core::hresult FrontPanelImplementation::SetLED(const string& ledIndicator, const uint32_t brightness, const string& color, const uint32_t red, const uint32_t green, const uint32_t blue, FrontPanelSuccess& success)
        {
            LOGINFO("[%s][%d]SetLED called - LED Indicator: %s, Brightness: %d, Color: %s, Red: %d, Green: %d, Blue: %d", __FUNCTION__, __LINE__, ledIndicator.c_str(), brightness, color.c_str(), red, green, blue);

            JsonObject properties;
            properties["ledIndicator"] = ledIndicator.c_str();
            properties["brightness"]   = brightness;
            properties["color"]        = color.c_str();
            properties["red"]          = red;
            properties["green"]        = green;
            properties["blue"]         = blue;

            // All DS/libds logic lives in CFrontPanel::setLED
            bool ok = CFrontPanel::instance()->setLED(properties);
            success.success = ok;
            return Core::ERROR_NONE;
        }

        /**
         * @brief Specifies a blinking pattern for an LED. This method returns immediately, but starts
         * a process of iterating through each element in the array and lighting the LED with the specified
         * brightness and color (if applicable) for the given duration (in milliseconds).
         *
         * @param[in] blinkInfo Object containing Indicator name, blink pattern and duration.
         * @ingroup SERVMGR_FRONTPANEL_API
         */
        void FrontPanelImplementation::setBlink(const JsonObject& blinkInfo)
        {
            CFrontPanel::instance()->setBlink(blinkInfo);
        }

        Core::hresult FrontPanelImplementation::SetBlink(const string& blinkInfo, FrontPanelSuccess& success)
        {
            LOGINFO("SetBlink called with blinkInfo: %s", blinkInfo.c_str());
            bool ok = false;
            try {
                JsonObject inputObj;
                inputObj.FromString(blinkInfo);
                // All DS/libds blink logic lives in CFrontPanel::setBlink
                setBlink(inputObj);
                ok = true;
            } catch (...) {
                LOGERR("Exception Caught during SetBlink");
                ok = false;
            }
            success.success = ok;
            return Core::ERROR_NONE;
        }
    } // namespace Plugin
} // namespace WPEFramework

#ifdef USE_DEVICESETTING_PLUGIN
namespace WPEFramework {
    namespace Plugin {
        void FrontPanelImplementation::OnDeviceSettingsActivated()
        {
            LOGINFO("OnDeviceSettingsActivated: setting FPD acquirer and loading config");

            // Give CFrontPanel a lambda to acquire the FPD interface on demand
            CFrontPanel::instance()->setFPDAcquirer([this]() {
                return AcquireSubInterface<Exchange::IDeviceSettingsFPD>();
            });

            // Eagerly load the FPD config store (indicator/color metadata) into CFrontPanel
            auto* fpd = AcquireSubInterface<Exchange::IDeviceSettingsFPD>();
            if (fpd) {
                CFrontPanel::instance()->updateFPDConfigStore(fpd);
                fpd->Register(&_dsFpdNotification);
                fpd->Release();
            } else {
                LOGERR("OnDeviceSettingsActivated: IDeviceSettingsFPD not yet available");
            }
        }

        void FrontPanelImplementation::OnDeviceSettingsDeactivated()
        {
            LOGINFO("OnDeviceSettingsDeactivated: clearing FPD interface");
            CFrontPanel::instance()->clearFPDInterface();
        }
    } // namespace Plugin
} // namespace WPEFramework
#endif
