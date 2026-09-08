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

#include "frontpanel.h"

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <cctype>

#if defined(HAS_API_POWERSTATE)
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

        static bool powerStatus = false;     //Check how this works on xi3 and rng's
        static bool started = false;
        static int m_numberOfBlinks = 0;
        static int m_maxNumberOfBlinkRepeats = 0;
        static int m_currentBlinkListIndex = 0;
        static PowerManagerInterfaceRef _powerManagerPlugin;

        static Core::TimerType<BlinkInfo> blinkTimer(64 * 1024, "BlinkTimer");

        namespace
        {

            struct Mapping
            {
                const char *IndicatorName;
                const char *SvcManagerName;
            };

            static struct Mapping name_mappings[] = {
                { "Record" , "record_led"},
                { "Message" , "data_led"},
                { "Power" , "power_led"},
                // TODO: add your mappings here
                // { <INDICATOR_NAME>, <SVC_MANAGER_API_NAME> },
                { 0,  0}
            };

            std::string svcToIndicatorName(const std::string &name)
            {
                const char *s = name.c_str();

                int i = 0;
                while (name_mappings[i].SvcManagerName)
                {
                    if (strcmp(s, name_mappings[i].SvcManagerName) == 0)
                        return name_mappings[i].IndicatorName;
                    i++;
                }
                return name;
            }


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

        /** Map an indicator name (e.g. "Message") or numeric index string to the DS FPDIndicator enum. */
        static Exchange::IDeviceSettingsFPD::FPDIndicator indicatorNameToDSIndicator(
            const std::string& name)
        {
            if (name == "Message")  return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE;
            if (name == "Power")    return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER;
            if (name == "Record")   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD;
            if (name == "Remote")   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE;
            if (name == "RfByPass") return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS;
            // Fallback: treat as numeric indicator index (e.g. "0"=Message, "1"=Power, "2"=Record)
            try {
                int idx = std::stoi(name);
                if (idx >= 0 && idx < static_cast<int>(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX))
                    return static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(idx);
            } catch (...) {}
            return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX;
        }

        /** Map a color name to its packed 0xRRGGBB value. Returns false if unsupported. */
        static bool colorNameToValue(const std::string& name, uint32_t& value)
        {
            std::string color = name;
            std::transform(color.begin(), color.end(), color.begin(),
                [](unsigned char character) { return std::tolower(character); });

            if (color == "white")       value = 0xFFFFFF;
            else if (color == "red")    value = 0xFF0000;
            else if (color == "green")  value = 0x00FF00;
            else if (color == "blue")   value = 0x0000FF;
            else if (color == "yellow") value = 0xFFFFE0;
            else if (color == "orange") value = 0xFF8C00;
            else return false;

            return true;
        }

        } // end anonymous namespace

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

                initDone = 1;
            }

            return s_instance;
        }

        void CFrontPanel::initializeFPD()
        {
            if (!m_fpdAcquirer) {
                LOGERR("initializeFPD: m_fpdAcquirer is null (DeviceSettings not yet activated)");
                return;
            }
            auto* fpd = m_fpdAcquirer();
            if (!fpd) {
                LOGERR("initializeFPD: IDeviceSettingsFPD interface not available");
                return;
            }

#if defined(HAS_API_POWERSTATE)
            {
                Core::hresult res = Core::ERROR_GENERAL;
                PowerState pwrStateCur  = Exchange::IPowerManager::POWER_STATE_UNKNOWN;
                PowerState pwrStatePrev = Exchange::IPowerManager::POWER_STATE_UNKNOWN;
                ASSERT (_powerManagerPlugin);
                if (_powerManagerPlugin) {
                    res = _powerManagerPlugin->GetPowerState(pwrStateCur, pwrStatePrev);
                    if (Core::ERROR_NONE == res)
                    {
                        if (pwrStateCur == Exchange::IPowerManager::POWER_STATE_ON)
                            powerStatus = true;
                    }
                    LOGINFO("pwrStateCur[%d] pwrStatePrev[%d] powerStatus[%d]", pwrStateCur, pwrStatePrev, powerStatus);
                }
            }
#endif

            uint32_t bright = 0;
            if (fpd->GetFPDBrightness(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER,
                    bright, false) == Core::ERROR_NONE)
                globalLedBrightness = static_cast<int>(bright);
            LOGINFO("Power light brightness, %d, power status %d", globalLedBrightness, powerStatus);

            profileType = searchRdkProfile();
            if (TV != profileType)
            {
                for (uint8_t i = 0;
                     i < static_cast<uint8_t>(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX);
                     ++i)
                {
                    auto ind = static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(i);
                    LOGWARN("Initializing light %s", dsIndicatorToSvcName(ind).c_str());
                    if (powerStatus)
                        fpd->SetFPDBrightness(ind, static_cast<uint32_t>(globalLedBrightness), false);

                    fpd->SetFPDState(ind, Exchange::IDeviceSettingsFPD::DS_FPD_STATE_OFF);
                }
            }
            else
            {
                LOGWARN("Power LED Initializing is not set since we continue with bootloader patern");
            }

            if (powerStatus)
                fpd->SetFPDState(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER,
                    Exchange::IDeviceSettingsFPD::DS_FPD_STATE_ON);

            fpd->Release();
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
            initDone = 0;
        }

        bool CFrontPanel::start()
        {
            LOGWARN("Front panel start");
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
        }

        int CFrontPanel::getBrightness()
        {
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    uint32_t bright = 0;
                    fpd->GetFPDBrightness(
                        Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, bright, false);
                    fpd->Release();
                    globalLedBrightness = static_cast<int>(bright);
                }
            }
            return globalLedBrightness;
        }

        bool CFrontPanel::powerOnLed(frontPanelIndicator fp_indicator)
        {
            stopBlinkTimer();
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
        }

        bool CFrontPanel::powerOffLed(frontPanelIndicator fp_indicator)
        {
            stopBlinkTimer();
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
            string ledIndicator = svcToIndicatorName(parameters["ledIndicator"].String());

            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                        indicatorNameToDSIndicator(ledIndicator);
                    if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                        if (parameters.HasLabel("color") && !parameters["color"].String().empty()) {
                            uint32_t colorValue = 0;
                            if (!colorNameToValue(parameters["color"].String(), colorValue)) {
                                LOGERR("setLED: unsupported color '%s'", parameters["color"].String().c_str());
                                fpd->Release();
                                return false;
                            }

                            success = (fpd->SetFPDColor(dsInd, colorValue) == Core::ERROR_NONE);
                        } else if (parameters.HasLabel("red")) {
                            uint32_t red = 0, green = 0, blue = 0;
                            getNumberParameter("red",   red);
                            getNumberParameter("green", green);
                            getNumberParameter("blue",  blue);
                            const auto rc = fpd->SetFPDColor(dsInd,
                                ((red & 0xFFU) << 16) | ((green & 0xFFU) << 8) | (blue & 0xFFU));
                            success = (rc == Core::ERROR_NONE);
                        }
                        // Apply brightness
                        int brightness = -1;
                        if (parameters.HasLabel("brightness")) {
                            uint32_t uBright = static_cast<uint32_t>(-1);
                            getNumberParameter("brightness", uBright);
                            if (uBright != static_cast<uint32_t>(-1))
                                brightness = static_cast<int>(uBright);
                        }
                        if (brightness < 0) {
                            uint32_t bright = 0;
                            fpd->GetFPDBrightness(dsInd, bright, true);
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
        }

        void CFrontPanel::setBlink(const JsonObject& blinkInfo)
        {
            stopBlinkTimer();
            m_blinkList.clear();
            string ledIndicator = svcToIndicatorName(blinkInfo["ledIndicator"].String());
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
                LOGWARN("setBlink ledIndicator: %s iterations: %d brightness: %d duration: %d",
                    ledIndicator.c_str(), iterations, brightness, duration);

                frontPanelBlinkInfo.brightness = brightness;
                frontPanelBlinkInfo.durationInMs = duration;
                frontPanelBlinkInfo.colorValue = 0;
                if (frontPanelBlinkHash.HasLabel("color")) //color mode 2
                {
                    frontPanelBlinkInfo.colorName = frontPanelBlinkHash["color"].String();
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
        } // end CFrontPanel::setBlink

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
            if (!m_fpdAcquirer) {
                LOGERR("setBlinkLed: m_fpdAcquirer is null (DeviceSettings not yet activated)");
                return;
            }
            auto* fpd = m_fpdAcquirer();
            if (!fpd) {
                LOGERR("setBlinkLed: IDeviceSettingsFPD interface not available");
                return;
            }

            Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                indicatorNameToDSIndicator(blinkInfo.ledIndicator);
            if (dsInd == Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                LOGERR("setBlinkLed: unknown ledIndicator='%s'", blinkInfo.ledIndicator.c_str());
                fpd->Release();
                return;
            }

            if (blinkInfo.colorMode == 1) {
                fpd->SetFPDColor(dsInd, blinkInfo.colorValue);
            } else if (blinkInfo.colorMode == 2) {
                uint32_t colorValue = 0;
                if (colorNameToValue(blinkInfo.colorName, colorValue))
                    fpd->SetFPDColor(dsInd, colorValue);
                else
                    LOGWARN("setBlinkLed: unsupported color '%s'", blinkInfo.colorName.c_str());
            }

            int brightness = blinkInfo.brightness;
            if (brightness == -1) {
                uint32_t bright = 0;
                if (fpd->GetFPDBrightness(dsInd, bright, true) == Core::ERROR_NONE)
                    brightness = static_cast<int>(bright);
            }
            if (brightness >= 0)
                fpd->SetFPDBrightness(dsInd, static_cast<uint32_t>(brightness), false);

            fpd->Release();
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

        bool CFrontPanel::setBrightnessByName(const std::string& indicatorName, int brightness)
        {
            LOGINFO("setBrightnessByName: indicatorName='%s' brightness=%d", indicatorName.c_str(), brightness);
            stopBlinkTimer();
            if (!m_fpdAcquirer) {
                LOGERR("setBrightnessByName: m_fpdAcquirer is null (DeviceSettings not yet activated)");
                return false;
            }
            auto* fpd = m_fpdAcquirer();
            if (!fpd) {
                LOGERR("setBrightnessByName: IDeviceSettingsFPD interface not available");
                return false;
            }
            Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                indicatorNameToDSIndicator(indicatorName);
            LOGINFO("setBrightnessByName: dsInd=%d (MAX=%d)",
                static_cast<int>(dsInd),
                static_cast<int>(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX));
            bool ok = false;
            if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                auto rc = fpd->SetFPDBrightness(dsInd,
                    static_cast<uint32_t>(brightness), true);
                ok = (rc == Core::ERROR_NONE);
                LOGINFO("setBrightnessByName: SetFPDBrightness rc=%u ok=%s", rc, ok ? "true" : "false");
            } else {
                LOGERR("setBrightnessByName: unknown indicatorName='%s', no indicator found", indicatorName.c_str());
            }
            fpd->Release();
            return ok;
        }

        int CFrontPanel::getBrightnessByName(const std::string& indicatorName)
        {
            if (m_fpdAcquirer) {
                auto* fpd = m_fpdAcquirer();
                if (fpd) {
                    Exchange::IDeviceSettingsFPD::FPDIndicator dsInd =
                        indicatorNameToDSIndicator(indicatorName);
                    int result = globalLedBrightness;
                    if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                        uint32_t bright = 0;
                        if (fpd->GetFPDBrightness(dsInd, bright, false) == Core::ERROR_NONE)
                            result = static_cast<int>(bright);
                    }
                    fpd->Release();
                    return result;
                }
            }
            return globalLedBrightness;
        }

        void CFrontPanel::setFPDAcquirer(
            std::function<Exchange::IDeviceSettingsFPD*()> acquirer)
        {
            m_fpdAcquirer = std::move(acquirer);
        }

        void CFrontPanel::clearFPDInterface()
        {
            m_fpdAcquirer = nullptr;
        }

    }
}

/** @} */
/** @} */
