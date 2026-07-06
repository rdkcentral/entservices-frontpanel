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

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

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
        static std::vector<std::string> m_lights;
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

            std::string svcToIndicatorName(const std::string &name)
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

        /** Map an IARM name (e.g. "Message") or numeric index string to the DS FPDIndicator enum. */
        static Exchange::IDeviceSettingsFPD::FPDIndicator iarmNameToDSIndicator(
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
                        Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER, bright);
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
        }

        void CFrontPanel::setBlink(const JsonObject& blinkInfo)
        {
            stopBlinkTimer();
            string ledIndicator = svcToIndicatorName(blinkInfo["ledIndicator"].String());
            int iterations = 0;
            getNumberParameterObject(blinkInfo, "iterations", iterations);

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
            LOGINFO("setBrightnessByName: iarmName='%s' brightness=%d", iarmName.c_str(), brightness);
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
                iarmNameToDSIndicator(iarmName);
            LOGINFO("setBrightnessByName: dsInd=%d (MAX=%d)",
                static_cast<int>(dsInd),
                static_cast<int>(Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX));
            bool ok = false;
            if (dsInd != Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX) {
                auto rc = fpd->SetFPDBrightness(dsInd,
                    static_cast<uint32_t>(brightness), false);
                ok = (rc == Core::ERROR_NONE);
                LOGINFO("setBrightnessByName: SetFPDBrightness rc=%u ok=%s", rc, ok ? "true" : "false");
            } else {
                LOGERR("setBrightnessByName: unknown iarmName='%s', no indicator found", iarmName.c_str());
            }
            fpd->Release();
            return ok;
        }

        int CFrontPanel::getBrightnessByName(const std::string& iarmName)
        {
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
        }

        // ─── Front-panel lights enumeration ───────────────────────────────────────

        std::vector<std::string> CFrontPanel::getFrontPanelLights()
        {
            std::vector<std::string> lights;
            const auto& indicators = m_fpConfigStore.GetIndicators();
            for (size_t i = 0; i < indicators.size(); ++i) {
                std::string name = dsIndicatorToSvcName(
                    static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(indicators[i].id));
                if (!name.empty())
                    lights.push_back(name);
            }
            return lights;
        }

        JsonObject CFrontPanel::getFrontPanelLightsInfo()
        {
            JsonObject returnResult;
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
            return returnResult;
        }

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

    }
}

/** @} */
/** @} */
