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

#include "frontPanelIndicator.hpp"
#include "frontPanelConfig.hpp"
#include "frontPanelTextDisplay.hpp"

#include "libIBus.h"

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

    string iarm2svc(const string &name)
    {
        const char *s = name.c_str();

        int i = 0;
        while (name_mappings[i].IArmBusName)
        {
            if (strcmp(s, name_mappings[i].IArmBusName) == 0)
                return name_mappings[i].SvcManagerName;
            i++;
        }
        return name;
    }

    void getFrontPanelIndicatorInfo(device::FrontPanelIndicator &indicator,JsonObject &indicatorInfo)
    {
        JsonObject returnResult;
        int levels=0, min=0, max=0;
        string range;

        indicator.getBrightnessLevels(levels, min, max);
        range = "int";
        indicatorInfo["range"] = range;

        indicatorInfo["min"] = JsonValue(min);
        indicatorInfo["max"] = JsonValue(max);

        indicatorInfo["step"] = JsonValue((max-min)/levels);
        
        JsonArray availableColors;
        const device::List <device::FrontPanelIndicator::Color> colorsList = indicator.getSupportedColors();
        for (uint j = 0; j < colorsList.size(); j++)
        {
            availableColors.Add(colorsList.at(j).getName());
        }
        if (availableColors.Length() > 0)
        {
            indicatorInfo["colors"] = availableColors;
        }

        indicatorInfo["colorMode"] = indicator.getColorMode();
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

            CFrontPanel::instance()->deinitialize();

            _registeredEventHandlers = false;

            FrontPanelImplementation::_instance = nullptr;

        }

        Core::hresult FrontPanelImplementation::Configure(PluginHost::IShell* service)
        {
            InitializePowerManager(service);
            FrontPanelImplementation::_instance = this;
            CFrontPanel::instance(service);
            CFrontPanel::instance()->start();
            CFrontPanel::instance()->addEventObserver(this);

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
            CFrontPanel::instance()->stopBlinkTimer();
            bool ok = false;

            string fp_ind = svc2iarm(index);
            if (!fp_ind.empty())
            {
                
                try
                {
                    device::FrontPanelIndicator::getInstance(fp_ind.c_str()).setBrightness(int(brightness));
                    ok = true;
                }
                catch (...)
                {
                    ok = false;
                }
            }
            else if (brightness >= 0 && brightness <= 100)
            {
                LOGWARN("calling setBrightness");
                ok = CFrontPanel::instance()->setBrightness(brightness);
            }
            else
            {
                LOGWARN("Invalid brightnessLevel passed to method setBrightness CallMethod");
                ok = false;
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
            

            string fp_ind = svc2iarm(index);

            if (!fp_ind.empty())
            {

                
                try
                {
                    value = device::FrontPanelIndicator::getInstance(fp_ind.c_str()).getBrightness();
                }
                catch (...)
                {
                    LOGWARN("Exception thrown from ds while calling getBrightness");
                }
                
            }
            else
            {
                LOGWARN("calling getBrightness");
                value = CFrontPanel::instance()->getBrightness();
            }

            if (value >= 0)
            {
                brightness = value;
                ok = true;
            }
            else
            {
                brightness = -1;
                ok = false;
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
            std::vector<std::string> lights;
            device::List <device::FrontPanelIndicator> fpIndicators = device::FrontPanelConfig::getInstance().getIndicators();
            for (uint i = 0; i < fpIndicators.size(); i++)
            {
                string IndicatorNameIarm = fpIndicators.at(i).getName();
                string MappedName = iarm2svc(IndicatorNameIarm);
                if (MappedName != IndicatorNameIarm) lights.push_back(MappedName);
            }
            return lights;
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
            JsonObject indicatorInfo;
            string IndicatorNameIarm, MappedName;

            device::List <device::FrontPanelIndicator> fpIndicators = device::FrontPanelConfig::getInstance().getIndicators();
            for (uint i = 0; i < fpIndicators.size(); i++)
            {
                IndicatorNameIarm = fpIndicators.at(i).getName();
                MappedName = iarm2svc(IndicatorNameIarm);
                getFrontPanelIndicatorInfo(fpIndicators.at(i),indicatorInfo);
                if (MappedName != IndicatorNameIarm)
                {
                    returnResult[MappedName.c_str()] = indicatorInfo;
                }
                else
                {
                    returnResult[IndicatorNameIarm.c_str()] = indicatorInfo;
                }		    
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
            supportedLightsInfo = infoStr;
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
                // Parse the input string as JSON
                JsonObject inputObj;
                inputObj.FromString(blinkInfo);

                // Call setBlink with the parsed object
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
