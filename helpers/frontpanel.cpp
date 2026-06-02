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

/**
* @defgroup servicemanager
* @{
* @defgroup src
* @{
**/

//#define USE_DS //TODO - this was defined in servicemanager.pro for all STB builds.  Not sure where to put it except here for now
//#define HAS_API_POWERSTATE

#include "frontpanel.h"
#include <interfaces/IDeviceSettingsFPD.h>

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#if defined(HAS_API_POWERSTATE)
#include "libIBus.h"
#include <interfaces/IPowerManager.h>

using namespace WPEFramework;
using PowerState = WPEFramework::Exchange::IPowerManager::PowerState;
#endif

#include "UtilsJsonRpc.h"
#include "UtilsLogging.h"
#include "UtilssyncPersistFile.h"
#include "PowerManagerInterface.h"
#include "UtilsSearchRDKProfile.h"
#include "FrontPanelConfigStore.h"

#define FP_SETTINGS_FILE_JSON "/opt/fp_service_preferences.json"
#define DEVICESETTINGS_CALLSIGN "org.rdk.DeviceSettings"

/*
Requirement now
    Ability to get/set Led brightness
    Ability to power off/on a led

*/

namespace WPEFramework
{

    namespace Plugin
    {
        CFrontPanel* CFrontPanel::s_instance = NULL;
        PluginHost::IShell* CFrontPanel::m_service = nullptr;
        static int globalLedBrightness = 100;

        int CFrontPanel::initDone = 0;
        static bool isMessageLedOn = false;
        static bool isRecordLedOn = false;

        static bool powerStatus = false;     //Check how this works on xi3 and rng's
        static bool started = false;
        static int m_numberOfBlinks = 0;
        static int m_maxNumberOfBlinkRepeats = 0;
        static int m_currentBlinkListIndex = 0;
        static std::vector<std::string> m_lights;
        static std::vector<Exchange::IDeviceSettingsFPD::FPDIndicator> fpIndicators;
        static PowerManagerInterfaceRef _powerManagerPlugin;
        static Exchange::IDeviceSettingsFPD* _deviceSettingsFPD = nullptr;

        static Core::TimerType<BlinkInfo> blinkTimer(64 * 1024, "BlinkTimer");

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

            std::string svc2iarm(const std::string &name)
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

            bool toIndicator(const std::string& name, Exchange::IDeviceSettingsFPD::FPDIndicator& indicator)
            {
                const std::string mapped = svc2iarm(name);
                if (mapped == "Message") {
                    indicator = Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE;
                    return true;
                }
                if (mapped == "Power") {
                    indicator = Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER;
                    return true;
                }
                if (mapped == "Record") {
                    indicator = Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD;
                    return true;
                }
                if (mapped == "Remote") {
                    indicator = Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE;
                    return true;
                }
                if ((mapped == "RfByPass") || (mapped == "RFBYPASS") || (mapped == "rf_bypass")) {
                    indicator = Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS;
                    return true;
                }
                return false;
            }

            uint32_t toColorValue(const std::string& colorName)
            {
                if (colorName == "red") {
                    return 0xFF0000;
                }
                if (colorName == "green") {
                    return 0x00FF00;
                }
                if (colorName == "blue") {
                    return 0x0000FF;
                }
                if (colorName == "yellow") {
                    return 0xFFFF00;
                }
                if (colorName == "orange") {
                    return 0xFF8C00;
                }
                if (colorName == "white") {
                    return 0xFFFFFF;
                }
                return 0;
            }
        }

        CFrontPanel::CFrontPanel()
            : m_blinkTimer(this)
            , m_isBlinking(false)
        {
        }

        bool CFrontPanel::EnsureDeviceSettingsFPD()
        {
            if (_deviceSettingsFPD != nullptr) {
                return true;
            }

            if (m_service == nullptr) {
                return false;
            }

            _deviceSettingsFPD = m_service->QueryInterfaceByCallsign<Exchange::IDeviceSettingsFPD>(DEVICESETTINGS_CALLSIGN);
            if (_deviceSettingsFPD == nullptr) {
                LOGWARN("Failed to query DeviceSettings interface from callsign: %s", DEVICESETTINGS_CALLSIGN);
            }

            return (_deviceSettingsFPD != nullptr);
        }

        CFrontPanel* CFrontPanel::instance(PluginHost::IShell *service)
        {
            if (!initDone)
            {
                if (nullptr != service)
                {
                    m_service = service;
                    _powerManagerPlugin = PowerManagerInterfaceBuilder(_T("org.rdk.PowerManager"))
                                      .withIShell(service)
                                      .withRetryIntervalMS(200)
                                      .withRetryCount(25)
                                      .createInterface();

                    EnsureDeviceSettingsFPD();
                }
                if (!s_instance)
                    s_instance = new CFrontPanel;

                try
                {
                    fpIndicators.clear();
                    if (EnsureDeviceSettingsFPD()) {
                        FrontPanelConfigStore configStore;
                        if (LoadFrontPanelConfig(_deviceSettingsFPD, configStore) == true) {
                            fpIndicators = configStore.indicators;

                            m_lights.clear();
                            for (size_t i = 0; i < fpIndicators.size(); ++i) {
                                switch (fpIndicators[i]) {
                                case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE:
                                    m_lights.push_back("Message");
                                    break;
                                case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER:
                                    m_lights.push_back("Power");
                                    break;
                                case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD:
                                    m_lights.push_back("Record");
                                    break;
                                case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE:
                                    m_lights.push_back("Remote");
                                    break;
                                case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS:
                                    m_lights.push_back("RfByPass");
                                    break;
                                default:
                                    break;
                                }
                            }
                        }
                    }

#if defined(HAS_API_POWERSTATE)
                    {
                        Core::hresult res = Core::ERROR_GENERAL;
                        PowerState pwrStateCur = WPEFramework::Exchange::IPowerManager::POWER_STATE_UNKNOWN;
                        PowerState pwrStatePrev = WPEFramework::Exchange::IPowerManager::POWER_STATE_UNKNOWN;
                        ASSERT (_powerManagerPlugin);
                        if (_powerManagerPlugin) {
                            res = _powerManagerPlugin->GetPowerState(pwrStateCur, pwrStatePrev);
                            if (Core::ERROR_NONE == res)
                            {
                                if (pwrStateCur == WPEFramework::Exchange::IPowerManager::POWER_STATE_ON)
                                    powerStatus = true;
                            }
                            LOGINFO("pwrStateCur[%d] pwrStatePrev[%d] powerStatus[%d]", pwrStateCur, pwrStatePrev, powerStatus);
                        }
                    }
#endif

                    if (EnsureDeviceSettingsFPD()) {
                        uint32_t powerBrightness = 100;
                        if (_deviceSettingsFPD->GetFPDBrightness(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, powerBrightness) == Core::ERROR_NONE) {
                            globalLedBrightness = static_cast<int>(powerBrightness);
                        } else {
                            globalLedBrightness = 100;
                        }
                    }
                    LOGINFO("Power light brightness, %d, power status %d", globalLedBrightness, powerStatus);

		    profileType = searchRdkProfile();
            if ((TV != profileType) && EnsureDeviceSettingsFPD())
		    {
                        for (size_t i = 0; i < fpIndicators.size(); i++)
			{
                LOGWARN("Initializing indicator %d", static_cast<int>(fpIndicators.at(i)));
			    if (powerStatus)
                                _deviceSettingsFPD->SetFPDBrightness(fpIndicators.at(i), globalLedBrightness, false);

                _deviceSettingsFPD->SetFPDState(fpIndicators.at(i), Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
			}
		    }
		    else
		    {
                        LOGWARN("Power LED Initializing is not set since we continue with bootloader patern");
		    }

            if (powerStatus && EnsureDeviceSettingsFPD())
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);

                }
                catch (...)
                {
                    LOGERR("Exception Caught during [CFrontPanel::instance]\r\n");
                }
                initDone=1;
            }

            return s_instance;
        }


        void CFrontPanel::deinitialize()
        {

            s_instance->stop();
            
            if (_powerManagerPlugin) {
                _powerManagerPlugin.Reset();
            }
            if (_deviceSettingsFPD != nullptr) {
                _deviceSettingsFPD->Release();
                _deviceSettingsFPD = nullptr;
            }
            m_service = nullptr;
            if (s_instance) {
                delete s_instance;
                s_instance = nullptr;
            }
            initDone = 0;
        }

        bool CFrontPanel::start()
        {
            LOGWARN("Front panel start");
            try
            {
                if (powerStatus && EnsureDeviceSettingsFPD()) {
                    _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER,
                                                    Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                }
            }
            catch (...)
            {
                LOGERR("Frontpanel Exception Caught during [%s]\r\n", __func__);
            }
            if (!started)
            {
                m_numberOfBlinks = 0;
                m_maxNumberOfBlinkRepeats = 0;
                m_currentBlinkListIndex = 0;
                started = true;
            }
            return true;
        }

        bool CFrontPanel::stop()
        {
            stopBlinkTimer();
            return true;
        }

        void CFrontPanel::setPowerStatus(bool bPowerStatus)
        {
            powerStatus = bPowerStatus;
        }

        std::string CFrontPanel::getLastError()
        {
            return lastError_;
        }

        void CFrontPanel::addEventObserver(FrontPanelImplementation* o)
        {

            auto it = std::find(observers_.begin(), observers_.end(), o);

            if (observers_.end() == it)
                observers_.push_back(o);
        }

        void CFrontPanel::removeEventObserver(FrontPanelImplementation* o)
        {
            observers_.remove(o);
        }

        bool CFrontPanel::setBrightness(int fp_brightness)
        {
            stopBlinkTimer();
            globalLedBrightness = fp_brightness;

            if (EnsureDeviceSettingsFPD()) {
                try {
                    for (size_t i = 0; i < fpIndicators.size(); i++) {
                        _deviceSettingsFPD->SetFPDBrightness(fpIndicators.at(i), globalLedBrightness, true);
                    }
                }
                catch (...) {
                    LOGERR("Frontpanel Exception Caught during [%s]\r\n",__func__);
                }
            }

            powerOnLed(FRONT_PANEL_INDICATOR_ALL);
            return true;
        }

        bool CFrontPanel::setBrightness(const std::string& index, int fp_brightness)
        {
            Exchange::IDeviceSettingsFPD::FPDIndicator indicator;

            if (toIndicator(index, indicator) == true) {
                try {
                    if (EnsureDeviceSettingsFPD()) {
                        _deviceSettingsFPD->SetFPDBrightness(indicator, fp_brightness, true);
                        return true;
                    }
                }
                catch (...) {
                    LOGWARN("Exception thrown from ds while calling setBrightness");
                    return false;
                }
            }

            return setBrightness(fp_brightness);
        }

        int CFrontPanel::getBrightness()
        {
            try {
                if (EnsureDeviceSettingsFPD()) {
                    uint32_t brightness = 0;
                    if (_deviceSettingsFPD->GetFPDBrightness(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, brightness) == Core::ERROR_NONE) {
                        globalLedBrightness = static_cast<int>(brightness);
                    }
                }
                LOGWARN("Power light brightness, %d\n", globalLedBrightness);
            }
            catch (...) {
                LOGERR("Frontpanel Exception Caught during [%s]\r\n", __func__);
            }

            return globalLedBrightness;
        }

        int CFrontPanel::getBrightness(const std::string& index)
        {
            Exchange::IDeviceSettingsFPD::FPDIndicator indicator;

            if (toIndicator(index, indicator) == true) {
                try {
                    if (EnsureDeviceSettingsFPD()) {
                        uint32_t brightness = 0;
                        if (_deviceSettingsFPD->GetFPDBrightness(indicator, brightness) == Core::ERROR_NONE) {
                            return static_cast<int>(brightness);
                        }
                    }
                    return -1;
                }
                catch (...) {
                    LOGWARN("Exception thrown from ds while calling getBrightness");
                    return -1;
                }
            }

            return getBrightness();
        }

        bool CFrontPanel::powerOnLed(frontPanelIndicator fp_indicator)
        {
            stopBlinkTimer();
            try {
                if (EnsureDeviceSettingsFPD() && powerStatus) {
                    switch (fp_indicator) {
                    case FRONT_PANEL_INDICATOR_MESSAGE:
                        isMessageLedOn = true;
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        break;
                    case FRONT_PANEL_INDICATOR_RECORD:
                        isRecordLedOn = true;
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        break;
                    case FRONT_PANEL_INDICATOR_REMOTE:
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        break;
                    case FRONT_PANEL_INDICATOR_RFBYPASS:
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        break;
                    case FRONT_PANEL_INDICATOR_ALL:
                        if (isMessageLedOn) {
                            _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        }
                        if (isRecordLedOn) {
                            _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        }
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        break;
                    case FRONT_PANEL_INDICATOR_POWER:
			_deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                        break;
                    default:
                        LOGERR("Invalid Indicator %d", fp_indicator);
                    }
                }
            }
            catch (...)
            {
                LOGERR("FrontPanel Exception Caught during [%s]\r\n", __func__);
                return false;
            }
            return true;
        }

        bool CFrontPanel::powerOffLed(frontPanelIndicator fp_indicator)
        {
            stopBlinkTimer();
            try {
                if (EnsureDeviceSettingsFPD()) {
                    switch (fp_indicator) {
                    case FRONT_PANEL_INDICATOR_MESSAGE:
                        isMessageLedOn = false;
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                        break;
                    case FRONT_PANEL_INDICATOR_RECORD:
                        isRecordLedOn = false;
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                        break;
                    case FRONT_PANEL_INDICATOR_REMOTE:
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                        break;
                    case FRONT_PANEL_INDICATOR_RFBYPASS:
                        _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                        break;
                    case FRONT_PANEL_INDICATOR_ALL:
                        for (size_t i = 0; i < fpIndicators.size(); i++) {
                            _deviceSettingsFPD->SetFPDState(fpIndicators.at(i), Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                        }
                        break;
                    case FRONT_PANEL_INDICATOR_POWER:
		    _deviceSettingsFPD->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                        break;
                    default:
                        LOGERR("Invalid Indicator %d", fp_indicator);
                    }
                }
            }
            catch (...)
            {
                LOGERR("FrontPanel Exception Caught during [%s]\r\n", __func__);
                return false;
            }
            return true;
        }


        bool CFrontPanel::powerOffAllLed()
        {
            powerOffLed(FRONT_PANEL_INDICATOR_ALL);
            return true;
        }

        bool CFrontPanel::powerOnAllLed()
        {
            powerOnLed(FRONT_PANEL_INDICATOR_ALL);
            return true;
        }

        bool CFrontPanel::setLED(const JsonObject& parameters)
        {
            stopBlinkTimer();
            bool success = false;
            string ledIndicator = parameters["ledIndicator"].String();
            int brightness = -1;
            Exchange::IDeviceSettingsFPD::FPDIndicator indicator;

            if ((EnsureDeviceSettingsFPD() == false) || (toIndicator(ledIndicator, indicator) == false)) {
                return false;
            }

            if (parameters.HasLabel("brightness"))
                //brightness = properties["brightness"].Number();
                getNumberParameter("brightness", brightness);

            unsigned int color = 0;
            if (parameters.HasLabel("color") && !parameters["color"].String().empty()) //color mode 2
            {
                string colorString = parameters["color"].String();
                color = toColorValue(colorString);
                success = ((color != 0) && (_deviceSettingsFPD->SetFPDColor(indicator, color) == Core::ERROR_NONE));
            }
            else if (parameters.HasLabel("red")) //color mode 1
            {
                unsigned int red = 0, green = 0, blue = 0;

                getNumberParameter("red", red);
                getNumberParameter("green", green);
                getNumberParameter("blue", blue);

                color = (red << 16) | (green << 8) | blue;
                success = (_deviceSettingsFPD->SetFPDColor(indicator, color) == Core::ERROR_NONE);
            }

            LOGWARN("setLed ledIndicator: %s brightness: %d", parameters["ledIndicator"].String().c_str(), brightness);
            try {
                if (brightness == -1) {
                    uint32_t existingBrightness = 0;
                    if (_deviceSettingsFPD->GetFPDBrightness(indicator, existingBrightness) == Core::ERROR_NONE) {
                        brightness = static_cast<int>(existingBrightness);
                    }
                }

                if (_deviceSettingsFPD->SetFPDBrightness(indicator, brightness, false) == Core::ERROR_NONE) {
                    success = true;
                }
            }
            catch (...) {
                success = false;
            }
            return success;
        }

        void CFrontPanel::setBlink(const JsonObject& blinkInfo)
        {
            stopBlinkTimer();
            m_blinkList.clear();
            string ledIndicator = svc2iarm(blinkInfo["ledIndicator"].String());
            int iterations = 0;
            getNumberParameterObject(blinkInfo, "iterations", iterations);
            JsonArray patternList = blinkInfo["pattern"].Array();
            for (int i = 0; i < patternList.Length(); i++)
            {
                JsonObject frontPanelBlinkHash = patternList[i].Object();
                FrontPanelBlinkInfo frontPanelBlinkInfo;
                frontPanelBlinkInfo.ledIndicator = ledIndicator;
                int brightness = -1;
                if (frontPanelBlinkHash.HasLabel("brightness"))
                    getNumberParameterObject(frontPanelBlinkHash, "brightness", brightness);

                int duration = 0;
                getNumberParameterObject(frontPanelBlinkHash, "duration", duration);
                LOGWARN("setBlink ledIndicator: %s iterations: %d brightness: %d duration: %d", ledIndicator.c_str(), iterations, brightness, duration);
                frontPanelBlinkInfo.brightness = brightness;
                frontPanelBlinkInfo.durationInMs = duration;
                frontPanelBlinkInfo.colorValue = 0;
                if (frontPanelBlinkHash.HasLabel("color")) //color mode 2
                {
                    string color = frontPanelBlinkHash["color"].String();
                    frontPanelBlinkInfo.colorName = std::move(color);
                    frontPanelBlinkInfo.colorMode = 2;
                }
                else if (frontPanelBlinkHash.HasLabel("red")) //color mode 1
                {
                    unsigned int red = 0, green = 0, blue = 0;

                    getNumberParameterObject(frontPanelBlinkHash, "red", red);
                    getNumberParameterObject(frontPanelBlinkHash, "green", green);
                    getNumberParameterObject(frontPanelBlinkHash, "blue", blue);

                    frontPanelBlinkInfo.colorValue = (red << 16) | (green << 8) | blue;
                    frontPanelBlinkInfo.colorMode = 1;
                }
                else
                {
                    frontPanelBlinkInfo.colorMode = 0;
                }
                m_blinkList.push_back(std::move(frontPanelBlinkInfo));
            }
            startBlinkTimer(iterations);
        }

        void CFrontPanel::startBlinkTimer(int numberOfBlinkRepeats)
        {
            LOGWARN("startBlinkTimer numberOfBlinkRepeats: %d m_blinkList.length : %zu", numberOfBlinkRepeats, m_blinkList.size());
            stopBlinkTimer();
            m_numberOfBlinks = 0;
            m_isBlinking = true;
            m_maxNumberOfBlinkRepeats = numberOfBlinkRepeats;
            m_currentBlinkListIndex = 0;
            if (m_blinkList.size() > 0)
            {
                FrontPanelBlinkInfo blinkInfo = m_blinkList.at(0);
                setBlinkLed(blinkInfo);
                if (m_isBlinking)
                    blinkTimer.Schedule(Core::Time::Now().Add(blinkInfo.durationInMs), m_blinkTimer);
            }
        }

        void CFrontPanel::stopBlinkTimer()
        {
            m_isBlinking = false;
            blinkTimer.Revoke(m_blinkTimer);
        }

        void CFrontPanel::setBlinkLed(FrontPanelBlinkInfo blinkInfo)
        {
            std::string ledIndicator = blinkInfo.ledIndicator;
            int brightness = blinkInfo.brightness;
            Exchange::IDeviceSettingsFPD::FPDIndicator indicator;

            if ((EnsureDeviceSettingsFPD() == false) || (toIndicator(ledIndicator, indicator) == false)) {
                return;
            }

            try
            {
                if (blinkInfo.colorMode == 1)
                {
                    _deviceSettingsFPD->SetFPDColor(indicator, blinkInfo.colorValue);
                }
                else if (blinkInfo.colorMode == 2)
                {
                    const uint32_t colorValue = toColorValue(blinkInfo.colorName);
                    if (colorValue != 0) {
                        _deviceSettingsFPD->SetFPDColor(indicator, colorValue);
                    }
                }

            }
            catch (...)
            {}
            try
            {
                if (brightness == -1) {
                    uint32_t existingBrightness = 0;
                    if (_deviceSettingsFPD->GetFPDBrightness(indicator, existingBrightness) == Core::ERROR_NONE) {
                        brightness = static_cast<int>(existingBrightness);
                    }
                }

                _deviceSettingsFPD->SetFPDBrightness(indicator, brightness, false);
            }
            catch (...)
            {
                LOGWARN("Exception caught in setBlinkLed for setBrightness ");
            }
        }

        void CFrontPanel::onBlinkTimer()
        {
            m_currentBlinkListIndex++;
            bool blinkAgain = true;
            if ((size_t)m_currentBlinkListIndex >= m_blinkList.size())
            {
                blinkAgain = false;
                m_currentBlinkListIndex = 0;
                m_numberOfBlinks++;
                if (m_maxNumberOfBlinkRepeats < 0 || m_numberOfBlinks <= m_maxNumberOfBlinkRepeats)
                {
                    blinkAgain = true;
                }
            }
            if (blinkAgain)
            {
                FrontPanelBlinkInfo blinkInfo = m_blinkList.at(m_currentBlinkListIndex);
                setBlinkLed(blinkInfo);
                if (m_isBlinking)
                    blinkTimer.Schedule(Core::Time::Now().Add(blinkInfo.durationInMs), m_blinkTimer);
            }

            //if not blink again then the led color should stay on the LAST element in the array as stated in the spec
        }

        uint64_t BlinkInfo::Timed(const uint64_t scheduledTime)
        {

            uint64_t result = 0;
            m_frontPanel->onBlinkTimer();
            return(result);
        }

    }
}

/** @} */
/** @} */
