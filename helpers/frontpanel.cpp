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
#ifndef USE_DEVICESETTING_PLUGIN
#ifdef USE_DS
    #include "frontPanelConfig.hpp"
    #include "frontPanelTextDisplay.hpp"
    #include "manager.hpp"
#endif
#endif

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

#define FP_SETTINGS_FILE_JSON "/opt/fp_service_preferences.json"

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
        static int globalLedBrightness = 100;

        int CFrontPanel::initDone = 0;
#ifndef USE_DEVICESETTING_PLUGIN
        static bool isMessageLedOn = false;
        static bool isRecordLedOn = false;
#endif // !USE_DEVICESETTING_PLUGIN

        static bool powerStatus = false;     //Check how this works on xi3 and rng's
        static bool started = false;
        static int m_numberOfBlinks = 0;
        static int m_maxNumberOfBlinkRepeats = 0;
        static int m_currentBlinkListIndex = 0;
        static std::vector<std::string> m_lights;
#ifndef USE_DEVICESETTING_PLUGIN
        static device::List <device::FrontPanelIndicator> fpIndicators;
#endif
        static PowerManagerInterfaceRef _powerManagerPlugin;

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

#ifndef USE_DEVICESETTING_PLUGIN
        static void getFrontPanelIndicatorInfo(device::FrontPanelIndicator& indicator,
                                               JsonObject& indicatorInfo)
        {
            int levels = 0, min = 0, max = 0;
            indicator.getBrightnessLevels(levels, min, max);
            indicatorInfo["range"] = std::string("int");
            indicatorInfo["min"]   = JsonValue(min);
            indicatorInfo["max"]   = JsonValue(max);
            indicatorInfo["step"]  = JsonValue(levels > 0 ? (max - min) / levels : 1);

            JsonArray availableColors;
            const device::List<device::FrontPanelIndicator::Color> colorsList =
                indicator.getSupportedColors();
            for (uint j = 0; j < colorsList.size(); j++)
                availableColors.Add(colorsList.at(j).getName());
            if (availableColors.Length() > 0)
                indicatorInfo["colors"] = availableColors;

            indicatorInfo["colorMode"] = indicator.getColorMode();
        }
#endif // !USE_DEVICESETTING_PLUGIN

#ifdef USE_DEVICESETTING_PLUGIN
        /** Map the legacy frontPanelIndicator enum to the DS FPDIndicator enum. */
        static Exchange::IDeviceSettingsFPD::FPDIndicator legacyToDSIndicator(
            frontPanelIndicator ind)
        {
            switch (ind) {
            case FRONT_PANEL_INDICATOR_MESSAGE:  return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE;
            case FRONT_PANEL_INDICATOR_POWER:    return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER;
            case FRONT_PANEL_INDICATOR_RECORD:   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD;
            case FRONT_PANEL_INDICATOR_REMOTE:   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE;
            case FRONT_PANEL_INDICATOR_RFBYPASS: return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS;
            default:                              return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX;
            }
        }

        /** Map an IARM name (e.g. "Message") to the DS FPDIndicator enum. */
        static Exchange::IDeviceSettingsFPD::FPDIndicator iarmNameToDSIndicator(
            const std::string& name)
        {
            if (name == "Message")  return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE;
            if (name == "Power")    return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER;
            if (name == "Record")   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD;
            if (name == "Remote")   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE;
            if (name == "RfByPass") return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS;
            return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX;
        }

#endif // USE_DEVICESETTING_PLUGIN
        } // end anonymous namespace

#ifdef USE_DEVICESETTING_PLUGIN
        /*static*/ std::string CFrontPanel::dsIndicatorToSvcName(
            Exchange::IDeviceSettingsFPD::FPDIndicator ind)
        {
            switch (ind) {
            case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE:  return "data_led";
            case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER:    return "power_led";
            case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD:   return "record_led";
            case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE:   return "remote_led";
            case Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS: return "rfbypass_led";
            default:                                                        return "";
            }
        }
#endif // USE_DEVICESETTING_PLUGIN

        CFrontPanel::CFrontPanel()
            : m_blinkTimer(this)
            , m_isBlinking(false)
        {
        }

        CFrontPanel* CFrontPanel::instance(PluginHost::IShell *service)
        {
            if (!initDone)
            {
                if (nullptr != service)
                {
                    _powerManagerPlugin = PowerManagerInterfaceBuilder(_T("org.rdk.PowerManager"))
                                      .withIShell(service)
                                      .withRetryIntervalMS(200)
                                      .withRetryCount(25)
                                      .createInterface();
                }
                if (!s_instance)
                    s_instance = new CFrontPanel;
#if defined(USE_DS) && !defined(USE_DEVICESETTING_PLUGIN)
                try
                {
                    LOGINFO("Initializing device manager");
                    device::Manager::Initialize();

                    LOGINFO("Front panel init");
                    fpIndicators = device::FrontPanelConfig::getInstance().getIndicators();

                    for (uint i = 0; i < fpIndicators.size(); i++)
                    {
                        std::string IndicatorNameIarm = fpIndicators.at(i).getName();

                        auto it = std::find(m_lights.begin(), m_lights.end(), IndicatorNameIarm);
                        if (m_lights.end() == it)
                        {
                            m_lights.push_back(std::move(IndicatorNameIarm));
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

                    globalLedBrightness = device::FrontPanelIndicator::getInstance("Power").getBrightness();
                    LOGINFO("Power light brightness, %d, power status %d", globalLedBrightness, powerStatus);

		    profileType = searchRdkProfile();
		    if (TV != profileType)
		    {
                        for (uint i = 0; i < fpIndicators.size(); i++)
			{
                            LOGWARN("Initializing light %s", fpIndicators.at(i).getName().c_str());
			    if (powerStatus)
                                device::FrontPanelIndicator::getInstance(fpIndicators.at(i).getName()).setBrightness(globalLedBrightness, false);

			    device::FrontPanelIndicator::getInstance(fpIndicators.at(i).getName()).setState(false);
			}
		    }
		    else
		    {
                        LOGWARN("Power LED Initializing is not set since we continue with bootloader patern");
		    }

		    if (powerStatus)
                        device::FrontPanelIndicator::getInstance("Power").setState(true);

                }
                catch (...)
                {
                    LOGERR("Exception Caught during [CFrontPanel::instance]\r\n");
                }
                initDone=1;
#endif
            }

            return s_instance;
        }


        void CFrontPanel::deinitialize()
        {

            s_instance->stop();
            
            if (_powerManagerPlugin) {
                _powerManagerPlugin.Reset();
            }
            if (s_instance) {
                delete s_instance;
                s_instance = nullptr;
            }
#if defined(USE_DS) && !defined(USE_DEVICESETTING_PLUGIN)
            try
            {
                device::Manager::DeInitialize();
                LOGINFO("device::Manager::DeInitialize success");
            }
            catch(const std::exception& e)
            {
                LOGERR("device::Manager::DeInitialize failed, Exception: {%s}", e.what());
            }
#endif
            initDone = 0;
        }

        bool CFrontPanel::start()
        {
            LOGWARN("Front panel start");
#ifndef USE_DEVICESETTING_PLUGIN
            try
            {
                if (powerStatus)
                    device::FrontPanelIndicator::getInstance("Power").setState(true);

                device::List <device::FrontPanelIndicator> fpIndicators = device::FrontPanelConfig::getInstance().getIndicators();
                for (uint i = 0; i < fpIndicators.size(); i++)
                {
                    std::string IndicatorNameIarm = fpIndicators.at(i).getName();

                    auto it = std::find(m_lights.begin(), m_lights.end(), IndicatorNameIarm);
                    if (m_lights.end() == it)
                        m_lights.push_back(std::move(IndicatorNameIarm));
                }
            }
            catch (...)
            {
                LOGERR("Frontpanel Exception Caught during [%s]\r\n", __func__);
            }
#endif // !USE_DEVICESETTING_PLUGIN
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

#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    bool allOk = true;
                    for (uint8_t i = 0;
                         i < static_cast<uint8_t>(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX);
                         ++i) {
                        auto rc = fpd->SetFPDBrightness(
                            static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(i),
                            static_cast<uint32_t>(fp_brightness), true);
                        if (rc != Core::ERROR_NONE) allOk = false;
                    }
                    fpd->Release();
                    return allOk;
                }
            }
            return false;
#else
            try
            {
                for (uint i = 0; i < fpIndicators.size(); i++)
                {
                    device::FrontPanelIndicator::getInstance(fpIndicators.at(i).getName()).setBrightness(globalLedBrightness);
                }
            }
            catch (...)
            {
                LOGERR("Frontpanel Exception Caught during [%s]\r\n",__func__);
            }

            powerOnLed(FRONT_PANEL_INDICATOR_ALL);
            return true;
#endif
        }

        int CFrontPanel::getBrightness()
        {
#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    uint32_t bright = 0;
                    fpd->GetFPDBrightness(
                        Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, bright);
                    fpd->Release();
                    globalLedBrightness = static_cast<int>(bright);
                }
            }
            return globalLedBrightness;
#else
            try
            {
                globalLedBrightness = device::FrontPanelIndicator::getInstance("Power").getBrightness();
                LOGWARN("Power light brightness, %d\n", globalLedBrightness);
            }
            catch (...)
            {
                LOGERR("Frontpanel Exception Caught during [%s]\r\n", __func__);
            }

            return globalLedBrightness;
#endif
        }

        bool CFrontPanel::powerOnLed(frontPanelIndicator fp_indicator)
        {
            stopBlinkTimer();
#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    bool ok = true;
                    if (fp_indicator == FRONT_PANEL_INDICATOR_ALL) {
                        for (uint8_t i = 0;
                             i < static_cast<uint8_t>(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX);
                             ++i) {
                            auto rc = fpd->SetFPDState(
                                static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(i),
                                Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);
                            if (rc != Core::ERROR_NONE) ok = false;
                        }
                    } else {
                        auto dsInd = legacyToDSIndicator(fp_indicator);
                        if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                            ok = (fpd->SetFPDState(dsInd,
                                Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON) == Core::ERROR_NONE);
                        }
                    }
                    fpd->Release();
                    return ok;
                }
            }
            return false;
#else
            try
            {
                if (powerStatus)
                {
                    switch (fp_indicator)
                    {
                    case FRONT_PANEL_INDICATOR_MESSAGE:
                        isMessageLedOn = true;
                        device::FrontPanelIndicator::getInstance("Message").setState(true);
                        break;
                    case FRONT_PANEL_INDICATOR_RECORD:
                        isRecordLedOn = true;
                        device::FrontPanelIndicator::getInstance("Record").setState(true);
                        break;
                    case FRONT_PANEL_INDICATOR_REMOTE:
                        device::FrontPanelIndicator::getInstance("Remote").setState(true);
                        break;
                    case FRONT_PANEL_INDICATOR_RFBYPASS:
                        device::FrontPanelIndicator::getInstance("RfByPass").setState(true);
                        break;
                    case FRONT_PANEL_INDICATOR_ALL:
                        if (isMessageLedOn)
                            device::FrontPanelIndicator::getInstance("Message").setState(true);
                        if (isRecordLedOn)
                            device::FrontPanelIndicator::getInstance("Record").setState(true);
                        device::FrontPanelIndicator::getInstance("Power").setState(true);
                        break;
                    case FRONT_PANEL_INDICATOR_POWER:
			device::FrontPanelIndicator::getInstance("Power").setState(true);
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
#endif
        }

        bool CFrontPanel::powerOffLed(frontPanelIndicator fp_indicator)
        {
            stopBlinkTimer();
#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    bool ok = true;
                    if (fp_indicator == FRONT_PANEL_INDICATOR_ALL) {
                        for (uint8_t i = 0;
                             i < static_cast<uint8_t>(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX);
                             ++i) {
                            auto rc = fpd->SetFPDState(
                                static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(i),
                                Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                            if (rc != Core::ERROR_NONE) ok = false;
                        }
                    } else {
                        auto dsInd = legacyToDSIndicator(fp_indicator);
                        if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                            ok = (fpd->SetFPDState(dsInd,
                                Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF) == Core::ERROR_NONE);
                        }
                    }
                    fpd->Release();
                    return ok;
                }
            }
            return false;
#else
            try
            {
                switch (fp_indicator)
                {
                case FRONT_PANEL_INDICATOR_MESSAGE:
                    isMessageLedOn = false;
                    device::FrontPanelIndicator::getInstance("Message").setState(false);
                    break;
                case FRONT_PANEL_INDICATOR_RECORD:
                    isRecordLedOn = false;
                    device::FrontPanelIndicator::getInstance("Record").setState(false);
                    break;
                case FRONT_PANEL_INDICATOR_REMOTE:
                    device::FrontPanelIndicator::getInstance("Remote").setState(false);
                    break;
                case FRONT_PANEL_INDICATOR_RFBYPASS:
                    device::FrontPanelIndicator::getInstance("RfByPass").setState(false);
                    break;
                case FRONT_PANEL_INDICATOR_ALL:
                    for (uint i = 0; i < fpIndicators.size(); i++)
                    {
                        LOGWARN("powerOffLed for Indicator %s", fpIndicators.at(i).getName().c_str());
                        device::FrontPanelIndicator::getInstance(fpIndicators.at(i).getName()).setState(false);
                    }
                    break;
                case FRONT_PANEL_INDICATOR_POWER:
		    device::FrontPanelIndicator::getInstance("Power").setState(false);
                    break;
                default:
                    LOGERR("Invalid Indicator %d", fp_indicator);
                }
            }
            catch (...)
            {
                LOGERR("FrontPanel Exception Caught during [%s]\r\n", __func__);
                return false;
            }
            return true;
#endif
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
            string ledIndicator = svc2iarm(parameters["ledIndicator"].String());

#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                        iarmNameToDSIndicator(ledIndicator);
                    if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                        // Apply colour
                        if (parameters.HasLabel("color") &&
                            !parameters["color"].String().empty()) {
                            // Resolve via config store: first colour binding for this indicator
                            const auto& bindings = m_fpConfigStore.GetColorBindings();
                            const auto& colors   = m_fpConfigStore.GetColors();
                            uint32_t colorVal = 0;
                            for (size_t b = 0; b < bindings.size(); ++b) {
                                if (bindings[b].targetType == 0 &&
                                    bindings[b].targetId == static_cast<int32_t>(dsInd)) {
                                    for (size_t c = 0; c < colors.size(); ++c) {
                                        if (colors[c].id == bindings[b].colorId) {
                                            colorVal = colors[c].color;
                                            break;
                                        }
                                    }
                                    break;
                                }
                            }
                            fpd->SetFPDColor(dsInd, colorVal);
                            success = true;
                        } else if (parameters.HasLabel("red")) {
                            uint32_t red = 0, green = 0, blue = 0;
                            getNumberParameter("red",   red);
                            getNumberParameter("green", green);
                            getNumberParameter("blue",  blue);
                            fpd->SetFPDColor(dsInd,
                                ((red & 0xFFU) << 16) | ((green & 0xFFU) << 8) | (blue & 0xFFU));
                            success = true;
                        }
                        // Apply brightness
                        int brightness = -1;
                        if (parameters.HasLabel("brightness"))
                            getNumberParameter("brightness", brightness);
                        if (brightness < 0) {
                            uint32_t bright = 0;
                            fpd->GetFPDBrightness(dsInd, bright);
                            brightness = static_cast<int>(bright);
                        }
                        if (brightness >= 0) {
                            success = (fpd->SetFPDBrightness(dsInd,
                                static_cast<uint32_t>(brightness), false) == Core::ERROR_NONE);
                        }
                    }
                    fpd->Release();
                }
                return success;
            }
            return false;
#else
            int brightness = -1;

            if (parameters.HasLabel("brightness"))
                //brightness = properties["brightness"].Number();
                getNumberParameter("brightness", brightness);

            unsigned int color = 0;
            if (parameters.HasLabel("color") && !parameters["color"].String().empty()) //color mode 2
            {
                string colorString = parameters["color"].String();
                try
                {
                    device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).setColor(device::FrontPanelIndicator::Color::getInstance(colorString.c_str()), false);
                    success = true;
                }
                catch (...)
                {
                    success = false;
                }
            }
            else if (parameters.HasLabel("red")) //color mode 1
            {
                unsigned int red = 0, green = 0, blue = 0;

                getNumberParameter("red", red);
                getNumberParameter("green", green);
                getNumberParameter("blue", blue);

                color = (red << 16) | (green << 8) | blue;
                try
                {
                    device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).setColor(color);
                    success = true;
                }
                catch (...)
                {
                    success = false;
                }
            }

            LOGWARN("setLed ledIndicator: %s brightness: %d", parameters["ledIndicator"].String().c_str(), brightness);
            try
            {
                if (brightness == -1)
                    brightness = device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).getBrightness(true);

                device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).setBrightness(brightness, false);
                success = true;
            }
            catch (...)
            {
                success = false;
            }
            return success;
#endif
        }

        void CFrontPanel::setBlink(const JsonObject& blinkInfo)
        {
            stopBlinkTimer();
            string ledIndicator = svc2iarm(blinkInfo["ledIndicator"].String());
            int iterations = 0;
            getNumberParameterObject(blinkInfo, "iterations", iterations);

#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                        iarmNameToDSIndicator(ledIndicator);
                    if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                        uint32_t blinkDuration = 0;
                        JsonArray patternList = blinkInfo["pattern"].Array();
                        if (patternList.Length() > 0) {
                            JsonObject firstEntry = patternList[0].Object();
                            int duration = 0;
                            getNumberParameterObject(firstEntry, "duration", duration);
                            blinkDuration = static_cast<uint32_t>(duration);
                        }
                        fpd->SetFPDBlink(dsInd, blinkDuration,
                            static_cast<uint32_t>(iterations > 0 ? iterations : 1));
                    }
                    fpd->Release();
                }
            }
        } // end CFrontPanel::setBlink
#else
            m_blinkList.clear();
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
#endif // !USE_DEVICESETTING_PLUGIN

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
#ifndef USE_DEVICESETTING_PLUGIN
            std::string ledIndicator = blinkInfo.ledIndicator;
            int brightness = blinkInfo.brightness;
            try
            {
                if (blinkInfo.colorMode == 1)
                {
                    device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).setColor(blinkInfo.colorValue, false);
                }
                else if (blinkInfo.colorMode == 2)
                {
                    device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).setColor(device::FrontPanelIndicator::Color::getInstance(blinkInfo.colorName.c_str()), false);
                }

            }
            catch (...)
            {}
            try
            {
                if (brightness == -1)
                    brightness = device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).getBrightness(true);

                device::FrontPanelIndicator::getInstance(ledIndicator.c_str()).setBrightness(brightness, false);
            }
            catch (...)
            {
                LOGWARN("Exception caught in setBlinkLed for setBrightness ");
            }
#endif // !USE_DEVICESETTING_PLUGIN
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

        // ─── Per-indicator brightness helpers ─────────────────────────────────────

        bool CFrontPanel::setBrightnessByName(const std::string& iarmName, int brightness)
        {
            stopBlinkTimer();
#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                        iarmNameToDSIndicator(iarmName);
                    bool ok = false;
                    if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                        ok = (fpd->SetFPDBrightness(dsInd,
                                static_cast<uint32_t>(brightness), false) == Core::ERROR_NONE);
                    }
                    fpd->Release();
                    return ok;
                }
            }
            return false;
#else
            try {
                device::FrontPanelIndicator::getInstance(iarmName.c_str()).setBrightness(brightness);
                return true;
            } catch (...) {
                LOGERR("Exception in setBrightnessByName for %s", iarmName.c_str());
                return false;
            }
#endif
        }

        int CFrontPanel::getBrightnessByName(const std::string& iarmName)
        {
#ifdef USE_DEVICESETTING_PLUGIN
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                        iarmNameToDSIndicator(iarmName);
                    int result = globalLedBrightness;
                    if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                        uint32_t bright = 0;
                        if (fpd->GetFPDBrightness(dsInd, bright) == Core::ERROR_NONE)
                            result = static_cast<int>(bright);
                    }
                    fpd->Release();
                    return result;
                }
            }
            return globalLedBrightness;
#else
            try {
                return device::FrontPanelIndicator::getInstance(iarmName.c_str()).getBrightness();
            } catch (...) {
                LOGWARN("Exception in getBrightnessByName for %s", iarmName.c_str());
                return globalLedBrightness;
            }
#endif
        }

        // ─── Front-panel lights enumeration ───────────────────────────────────────

        std::vector<std::string> CFrontPanel::getFrontPanelLights()
        {
            std::vector<std::string> lights;
#ifdef USE_DEVICESETTING_PLUGIN
            const auto& indicators = m_fpConfigStore.GetIndicators();
            for (size_t i = 0; i < indicators.size(); ++i) {
                std::string name = dsIndicatorToSvcName(
                    static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(indicators[i].id));
                if (!name.empty())
                    lights.push_back(name);
            }
#else
            try {
                device::List<device::FrontPanelIndicator> fpIndicators =
                    device::FrontPanelConfig::getInstance().getIndicators();
                for (uint i = 0; i < fpIndicators.size(); i++) {
                    std::string iarmName = fpIndicators.at(i).getName();
                    // Only include indicators that have a svc-manager name
                    bool found = false;
                    for (int m = 0; name_mappings[m].IArmBusName; ++m) {
                        if (iarmName == name_mappings[m].IArmBusName) {
                            lights.push_back(name_mappings[m].SvcManagerName);
                            found = true;
                            break;
                        }
                    }
                    (void)found;
                }
            } catch (...) {
                LOGERR("Exception in CFrontPanel::getFrontPanelLights");
            }
#endif
            return lights;
        }

        JsonObject CFrontPanel::getFrontPanelLightsInfo()
        {
            JsonObject returnResult;
#ifdef USE_DEVICESETTING_PLUGIN
            const auto& indicators    = m_fpConfigStore.GetIndicators();
            const auto& colorBindings = m_fpConfigStore.GetColorBindings();
            const auto& colors        = m_fpConfigStore.GetColors();

            for (size_t i = 0; i < indicators.size(); ++i) {
                const auto& ind = indicators[i];
                std::string svcName = dsIndicatorToSvcName(
                    static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(ind.id));
                if (svcName.empty()) continue;

                JsonObject info;
                info["range"]     = std::string("int");
                info["min"]       = JsonValue(ind.minBrightness);
                info["max"]       = JsonValue(ind.maxBrightness);
                int step = (ind.levels > 0 && ind.maxBrightness > ind.minBrightness)
                    ? (ind.maxBrightness - ind.minBrightness) / ind.levels : 1;
                info["step"]      = JsonValue(step);
                info["colorMode"] = JsonValue(ind.colorMode);

                if (ind.colorMode > 0) {
                    JsonArray availableColors;
                    for (size_t b = 0; b < colorBindings.size(); ++b) {
                        if (colorBindings[b].targetType == 0 &&
                            colorBindings[b].targetId == ind.id) {
                            for (size_t c = 0; c < colors.size(); ++c) {
                                if (colors[c].id == colorBindings[b].colorId) {
                                    char hexBuf[16];
                                    snprintf(hexBuf, sizeof(hexBuf), "#%06X",
                                             colors[c].color & 0xFFFFFFU);
                                    availableColors.Add(std::string(hexBuf));
                                    break;
                                }
                            }
                        }
                    }
                    if (availableColors.Length() > 0)
                        info["colors"] = availableColors;
                }
                returnResult[svcName.c_str()] = info;
            }
#else
            try {
                device::List<device::FrontPanelIndicator> fpIndicators =
                    device::FrontPanelConfig::getInstance().getIndicators();
                for (uint i = 0; i < fpIndicators.size(); i++) {
                    std::string iarmName  = fpIndicators.at(i).getName();
                    std::string svcName   = iarmName;
                    for (int m = 0; name_mappings[m].IArmBusName; ++m) {
                        if (iarmName == name_mappings[m].IArmBusName) {
                            svcName = name_mappings[m].SvcManagerName;
                            break;
                        }
                    }
                    JsonObject indicatorInfo;
                    getFrontPanelIndicatorInfo(fpIndicators.at(i), indicatorInfo);
                    returnResult[svcName.c_str()] = indicatorInfo;
                }
            } catch (...) {
                LOGERR("Exception in CFrontPanel::getFrontPanelLightsInfo");
            }
#endif
            return returnResult;
        }

#ifdef USE_DEVICESETTING_PLUGIN
        // ─── DS lifecycle management ──────────────────────────────────────────────

        void CFrontPanel::setFPDAcquirer(
            std::function<Exchange::IDeviceSettingsFPD*()> acquirer)
        {
            m_fpdAcquirer = std::move(acquirer);
        }

        void CFrontPanel::updateFPDConfigStore(Exchange::IDeviceSettingsFPD* fpd)
        {
            if (fpd) {
                LoadFrontPanelConfig(fpd, m_fpConfigStore);
            }
        }

        void CFrontPanel::clearFPDInterface()
        {
            m_fpdAcquirer = nullptr;
            m_fpConfigStore.Clear();
        }
#endif // USE_DEVICESETTING_PLUGIN

    }
}

/** @} */
/** @} */
