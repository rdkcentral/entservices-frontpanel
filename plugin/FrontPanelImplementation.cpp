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
#include <cmath>
#include "FrontPanelConfigStore.h"

#include "UtilsJsonRpc.h"

#define SERVICE_NAME "FrontPanelService"
#define METHOD_FP_SET_BRIGHTNESS "setBrightness"
#define METHOD_FP_GET_BRIGHTNESS "getBrightness"
#define METHOD_FP_POWER_LED_ON "powerLedOn"
#define METHOD_FP_POWER_LED_OFF "powerLedOff"
#define METHOD_GET_FRONT_PANEL_LIGHTS "getFrontPanelLights"
#define METHOD_FP_SET_LED "setLED"
#define METHOD_FP_SET_BLINK "setBlink"

#define DEVICESETTINGS_CALLSIGN "org.rdk.DeviceSettings"

#define API_VERSION_NUMBER_MAJOR 1
#define API_VERSION_NUMBER_MINOR 0
#define API_VERSION_NUMBER_PATCH 6

using PowerState = WPEFramework::Exchange::IPowerManager::PowerState;


namespace
{
    string ToServiceName(const Exchange::IDeviceSettingsFPD::FPDIndicator indicator)
    {
        switch (indicator) {
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE:
            return DATA_LED;
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD:
            return RECORD_LED;
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER:
            return POWER_LED;
        default:
            break;
        }

        return string();
    }

    string ToIarmName(const Exchange::IDeviceSettingsFPD::FPDIndicator indicator)
    {
        switch (indicator) {
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE:
            return "Message";
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD:
            return "Record";
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER:
            return "Power";
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE:
            return "Remote";
        case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS:
            return "RfByPass";
        default:
            break;
        }

        return string();
    }

    string ColorName(const uint32_t color)
    {
        switch (color) {
        case 0xFF0000:
            return "red";
        case 0x00FF00:
            return "green";
        case 0x0000FF:
            return "blue";
        case 0xFFFF00:
            return "yellow";
        case 0xFF8C00:
            return "orange";
        case 0xFFFFFF:
            return "white";
        default:
            break;
        }

        return string();
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
        , _service(nullptr)
        , _deviceSettingsFPD(nullptr)
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

            if (CFrontPanel::initDone) {
                CFrontPanel::deinitialize();
            }

            ReleaseDeviceSettings();

            _registeredEventHandlers = false;

            FrontPanelImplementation::_instance = nullptr;

        }

        Core::hresult FrontPanelImplementation::Configure(PluginHost::IShell* service)
        {
            _service = service;
            InitializePowerManager(service);
            FrontPanelImplementation::_instance = this;

            ReleaseDeviceSettings();
            if (EnsureDeviceSettingsFPD() == false) {
                LOGWARN("Failed to query DeviceSettings interface from callsign: %s", DEVICESETTINGS_CALLSIGN);
                return Core::ERROR_UNAVAILABLE;
            }

            if (RefreshFrontPanelConfig() == false) {
                LOGWARN("Failed to load frontpanel configuration from DeviceSettings");
            }

            CFrontPanel::instance(service);
            CFrontPanel::instance()->start();
            CFrontPanel::instance()->addEventObserver(this);

            return Core::ERROR_NONE;
        }

        bool FrontPanelImplementation::EnsureDeviceSettingsFPD()
        {
            if (_deviceSettingsFPD != nullptr) {
                return true;
            }

            if (_service == nullptr) {
                return false;
            }

            _deviceSettingsFPD = _service->QueryInterfaceByCallsign<Exchange::IDeviceSettingsFPD>(DEVICESETTINGS_CALLSIGN);
            if (_deviceSettingsFPD == nullptr) {
                LOGWARN("Failed to query DeviceSettings interface from callsign: %s", DEVICESETTINGS_CALLSIGN);
            }

            return (_deviceSettingsFPD != nullptr);
        }

        void FrontPanelImplementation::ReleaseDeviceSettings()
        {
            std::lock_guard<std::mutex> lock(_configMutex);
            _supportedLights.clear();
            _indicatorConfigByName.clear();
            _colorValueById.clear();

            if (_deviceSettingsFPD != nullptr) {
                _deviceSettingsFPD->Release();
                _deviceSettingsFPD = nullptr;
            }
        }

        bool FrontPanelImplementation::RefreshFrontPanelConfig()
        {
            if (EnsureDeviceSettingsFPD() == false) {
                return false;
            }

            FrontPanelConfigStore configStore;
            if (LoadFrontPanelConfig(_deviceSettingsFPD, configStore) == false) {
                return false;
            }

            std::vector<string> supportedLights;
            std::unordered_map<string, IndicatorConfig> indicatorConfigByName;
            for (const auto& indicatorConfig : configStore.indicatorConfigs) {
                if ((indicatorConfig.id < Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE) ||
                    (indicatorConfig.id >= Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX)) {
                    continue;
                }

                const auto indicator = static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(indicatorConfig.id);
                const string iarmName = ToIarmName(indicator);
                if (iarmName.empty() == true) {
                    continue;
                }

                const string lightName = ToServiceName(indicator);
                const string infoName = lightName.empty() ? iarmName : lightName;

                IndicatorConfig lightConfig;
                lightConfig.indicator = indicator;
                lightConfig.minBrightness = indicatorConfig.minBrightness;
                lightConfig.maxBrightness = indicatorConfig.maxBrightness;
                lightConfig.levels = indicatorConfig.levels;
                lightConfig.colorMode = indicatorConfig.colorMode;

                const auto mappedColors = configStore.colorsByIndicatorId.find(indicatorConfig.id);
                if (mappedColors != configStore.colorsByIndicatorId.end()) {
                    lightConfig.colors = mappedColors->second;
                } else {
                    for (const auto& colorEntry : configStore.colorValueById) {
                        lightConfig.colors.push_back(colorEntry.second);
                    }
                }

                if (lightName.empty() == false) {
                    supportedLights.push_back(lightName);
                }
                indicatorConfigByName.emplace(infoName, std::move(lightConfig));
            }

            std::lock_guard<std::mutex> lock(_configMutex);
            _supportedLights = std::move(supportedLights);
            _indicatorConfigByName = std::move(indicatorConfigByName);
            _colorValueById = std::move(configStore.colorValueById);
            return true;
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
            (void) currentState;

            if(newState == WPEFramework::Exchange::IPowerManager::POWER_STATE_ON) {
                CFrontPanel::instance()->setPowerStatus(true);
            }
            else {
                CFrontPanel::instance()->setPowerStatus(false);
            }
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
            CFrontPanel::instance()->stopBlinkTimer();
            bool ok = false;

            if ((index == DATA_LED) || (index == RECORD_LED) || (index == POWER_LED)) {
                ok = CFrontPanel::instance()->setBrightness(index, static_cast<int>(brightness));
            } else if (brightness <= 100) {
                ok = CFrontPanel::instance()->setBrightness(static_cast<int>(brightness));
            }

            success.success = ok;
            return ok ? Core::ERROR_NONE : Core::ERROR_GENERAL;
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

            if ((index == DATA_LED) || (index == RECORD_LED) || (index == POWER_LED)) {
                value = CFrontPanel::instance()->getBrightness(index);
            } else {
                LOGWARN("calling getBrightness");
                value = CFrontPanel::instance()->getBrightness();
            }

            if (value >= 0) {
                brightness = static_cast<uint32_t>(value);
                ok = true;
            } else {
                brightness = 0;
            }

            success = ok;
            return ok ? Core::ERROR_NONE : Core::ERROR_GENERAL;
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
            return ok ? Core::ERROR_NONE : Core::ERROR_GENERAL;
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
            return ok ? Core::ERROR_NONE : Core::ERROR_GENERAL;
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
            std::lock_guard<std::mutex> lock(_configMutex);
            return _supportedLights;
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
            JsonObject returnResult;
            std::lock_guard<std::mutex> lock(_configMutex);

            for (const auto& it : _indicatorConfigByName) {
                const string& name = it.first;
                const IndicatorConfig& cfg = it.second;

                JsonObject indicatorInfo;
                indicatorInfo["range"] = string("int");
                indicatorInfo["min"] = cfg.minBrightness;
                indicatorInfo["max"] = cfg.maxBrightness;
                indicatorInfo["step"] = ((cfg.levels > 0) ? std::max(1, (cfg.maxBrightness - cfg.minBrightness) / cfg.levels) : 1);
                indicatorInfo["colorMode"] = cfg.colorMode;

                JsonArray availableColors;
                for (const auto color : cfg.colors) {
                    const string colorName = ColorName(color);
                    if (colorName.empty() == false) {
                        availableColors.Add(colorName);
                    }
                }

                if (availableColors.Length() > 0) {
                    indicatorInfo["colors"] = availableColors;
                }

                returnResult[name.c_str()] = indicatorInfo;
            }

            return returnResult;
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
            properties["brightness"] = brightness;
            properties["color"] = color.c_str();
            properties["red"] = red;
            properties["green"] = green;
            properties["blue"] = blue;

            bool ok = CFrontPanel::instance()->setLED(properties);

            success.success = ok;
            return ok ? Core::ERROR_NONE : Core::ERROR_GENERAL;
        }

        void FrontPanelImplementation::setBlink(const JsonObject& blinkInfo)
        {
            CFrontPanel::instance()->setBlink(blinkInfo);
        }

        /**
         * @brief Specifies a blinking pattern for an LED. This method returns immediately, but starts
         * a process of iterating through each element in the array and lighting the LED with the specified
         * brightness and color (if applicable) for the given duration (in milliseconds).
         *
         * @param[in] blinkInfo Object containing Indicator name, blink pattern and duration.
         * @ingroup SERVMGR_FRONTPANEL_API
         */
        Core::hresult FrontPanelImplementation::SetBlink(const string& blinkInfo, FrontPanelSuccess& success)
        {
            LOGINFO("SetBlink called with blinkInfo: %s", blinkInfo.c_str());
            bool ok = false;

            try {
                JsonObject inputObj;
                inputObj.FromString(blinkInfo);

                setBlink(inputObj);
                ok = true;
            } catch (...) {
                ok = false;
            }

            success.success = ok;
            return ok ? Core::ERROR_NONE : Core::ERROR_GENERAL;
        }

    } // namespace Plugin
} // namespace WPEFramework
