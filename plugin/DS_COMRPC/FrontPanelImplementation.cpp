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
 * @file DS_COMRPC/FrontPanelImplementation.cpp
 *
 * @brief COM-RPC DeviceSettings path for the FrontPanel plugin.
 *
 * Compiled when USE_DEVICESETTING_PLUGIN is defined.
 * Connects to entservices-devicesettings via COM-RPC (IDeviceSettingsFPD).
 * All actual FP HAL operations are delegated to CFrontPanel (helpers/DS_COMRPC/)
 * which calls IDeviceSettingsFPD via the acquirer lambda set in
 * OnDeviceSettingsActivated().
 */

#include "FrontPanelImplementation.h"
#include "frontpanel.h"
#include <algorithm>

#include "UtilsJsonRpc.h"

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
        { 0, 0}
    };

    string svcToIndicatorName(const string &name)
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
            , _dsFpdNotification(*this)   // COM-RPC notification sink
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

            // Unregister FPD notification and close COM-RPC link
            {
                auto* fpd = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsFPD>();
                if (fpd) {
                    fpd->Unregister(&_dsFpdNotification);
                    fpd->Release();
                }
            }
            DSHelper::Close();
            _registeredEventHandlers = false;
            FrontPanelImplementation::_instance = nullptr;
        }

        Core::hresult FrontPanelImplementation::Configure(PluginHost::IShell* service)
        {
            InitializePowerManager(service);
            FrontPanelImplementation::_instance = this;
            // Open COM-RPC link to DeviceSettings plugin
            // CFrontPanel is initialised lazily once OnDeviceSettingsActivated fires
            DSHelper::Open(service);
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
            // The DeviceSettings plugin manages front panel power state internally;
            // no explicit CFrontPanel gate is needed here.
            LOGINFO("onPowerModeChanged: newState=%d (DS COM-RPC path)", static_cast<int>(newState));
        }

        void FrontPanelImplementation::registerEventHandlers()
        {
            ASSERT(_powerManagerPlugin);
            if (!_registeredEventHandlers && _powerManagerPlugin) {
                _registeredEventHandlers = true;
                _powerManagerPlugin->Register(_pwrMgrNotification.baseInterface<Exchange::IPowerManager::IModeChangedNotification>());
            }
        }

        // ── IFrontPanel method implementations ─────────────────────────────────
        // All delegate to CFrontPanel, which calls IDeviceSettingsFPD via the
        // acquirer lambda installed in OnDeviceSettingsActivated().

        Core::hresult FrontPanelImplementation::SetBrightness(const string& index, const uint32_t brightness, FrontPanelSuccess& success)
        {
            LOGINFO("SetBrightness: index=%s brightness=%u", index.c_str(), brightness);
            bool ok = false;
            string fp_ind = svcToIndicatorName(index);
            LOGINFO("SetBrightness: resolved indicator name='%s'", fp_ind.c_str());
            if (!fp_ind.empty())
                ok = CFrontPanel::instance()->setBrightnessByName(fp_ind, static_cast<int>(brightness));
            else if (brightness <= 100)
                ok = CFrontPanel::instance()->setBrightness(static_cast<int>(brightness));
            else
                LOGWARN("Invalid brightnessLevel passed to SetBrightness");
            LOGINFO("SetBrightness: result=%s", ok ? "success" : "failed");
            success.success = ok;
            return Core::ERROR_NONE;
        }

        Core::hresult FrontPanelImplementation::GetBrightness(const string& index, uint32_t& brightness, bool& success)
        {
            LOGINFO("GetBrightness: index=%s", index.c_str());
            int value = -1;
            string fp_ind = svcToIndicatorName(index);
            if (!fp_ind.empty())
                value = CFrontPanel::instance()->getBrightnessByName(fp_ind);
            else
                value = CFrontPanel::instance()->getBrightness();

            if (value >= 0) {
                brightness = static_cast<uint32_t>(value);
                success = true;
            } else {
                brightness = static_cast<uint32_t>(-1);
                success = false;
            }
            return Core::ERROR_NONE;
        }

        Core::hresult FrontPanelImplementation::PowerLedOn(const string& index, FrontPanelSuccess& success)
        {
            bool ok = false;
            if (index == DATA_LED)
                ok = CFrontPanel::instance()->powerOnLed(FRONT_PANEL_INDICATOR_MESSAGE);
            else if (index == RECORD_LED)
                ok = CFrontPanel::instance()->powerOnLed(FRONT_PANEL_INDICATOR_RECORD);
            else if (index == POWER_LED)
                ok = CFrontPanel::instance()->powerOnLed(FRONT_PANEL_INDICATOR_POWER);
            success.success = ok;
            return Core::ERROR_NONE;
        }

        Core::hresult FrontPanelImplementation::PowerLedOff(const string& index, FrontPanelSuccess& success)
        {
            bool ok = false;
            if (index == DATA_LED)
                ok = CFrontPanel::instance()->powerOffLed(FRONT_PANEL_INDICATOR_MESSAGE);
            else if (index == RECORD_LED)
                ok = CFrontPanel::instance()->powerOffLed(FRONT_PANEL_INDICATOR_RECORD);
            else if (index == POWER_LED)
                ok = CFrontPanel::instance()->powerOffLed(FRONT_PANEL_INDICATOR_POWER);
            success.success = ok;
            return Core::ERROR_NONE;
        }

        std::vector<std::string> FrontPanelImplementation::getFrontPanelLights()
        {
            // Read FPD indicator config directly from DSHelper — same pattern as DisplaySettings
            // calling DSHelper::getAudioPortEntries() etc. inline.
            std::vector<std::string> lights;
            const auto indicators = DSHelper::getFPDIndicators();
            for (size_t i = 0; i < indicators.size(); ++i) {
                std::string name = CFrontPanel::dsIndicatorToSvcName(
                    static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(indicators[i].id));
                if (!name.empty())
                    lights.push_back(name);
            }
            return lights;
        }

        JsonObject FrontPanelImplementation::getFrontPanelLightsInfo()
        {
            // Read FPD config directly from DSHelper — same pattern as DisplaySettings.
            JsonObject returnResult;
            const auto indicators    = DSHelper::getFPDIndicators();
            const auto colorBindings = DSHelper::getFPDColorBindings();
            const auto colors        = DSHelper::getFPDColors();

            for (size_t i = 0; i < indicators.size(); ++i) {
                const auto& ind = indicators[i];
                std::string svcName = CFrontPanel::dsIndicatorToSvcName(
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

        Core::hresult FrontPanelImplementation::GetFrontPanelLights(IFrontPanelLightsListIterator*& supportedLights, string& supportedLightsInfo, bool& success)
        {
            LOGINFO("[%s][%d]GetFrontPanelLights called", __FUNCTION__, __LINE__);
            std::vector<std::string> lights = getFrontPanelLights();

            JsonObject info = getFrontPanelLightsInfo();
            string infoStr;
            info.ToString(infoStr);
            supportedLightsInfo = std::move(infoStr);
            success = true;

            supportedLights = (Core::Service<RPC::IteratorType<Exchange::IFrontPanel::IFrontPanelLightsListIterator>>::Create<Exchange::IFrontPanel::IFrontPanelLightsListIterator>(lights));
            return Core::ERROR_NONE;
        }

        Core::hresult FrontPanelImplementation::SetLED(const string& ledIndicator, const uint32_t brightness, const string& color, const uint32_t red, const uint32_t green, const uint32_t blue, FrontPanelSuccess& success)
        {
            LOGINFO("[%s][%d]SetLED: %s brightness=%u", __FUNCTION__, __LINE__, ledIndicator.c_str(), brightness);
            JsonObject properties;
            properties["ledIndicator"] = ledIndicator.c_str();
            properties["brightness"]   = brightness;

            if (!color.empty()) {
                // Resolve the color name → RGB via DSHelper — same pattern as DisplaySettings
                // calling DSHelper::getAudioPortHandleEntries() etc. directly.
                // Map service-manager LED name → DS FPDIndicator inline.
                auto svcToDs = [](const std::string& n) -> Exchange::IDeviceSettingsFPD::FPDIndicator {
                    if (n == "power_led")    return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_POWER;
                    if (n == "record_led")   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RECORD;
                    if (n == "data_led")     return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE;
                    if (n == "remote_led")   return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_REMOTE;
                    if (n == "rfbypass_led") return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_RFBYPASS;
                    return Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX;
                };
                Exchange::IDeviceSettingsFPD::FPDIndicator dsInd = svcToDs(ledIndicator);
                uint32_t colorVal = 0;
                const auto bindings = DSHelper::getFPDColorBindings();
                const auto colors   = DSHelper::getFPDColors();
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
                // Pass the resolved color as RGB so CFrontPanel needs no config access.
                properties["color"]  = "";
                properties["red"]    = (colorVal >> 16) & 0xFFU;
                properties["green"]  = (colorVal >> 8)  & 0xFFU;
                properties["blue"]   = colorVal & 0xFFU;
            } else {
                properties["color"]  = "";
                properties["red"]    = red;
                properties["green"]  = green;
                properties["blue"]   = blue;
            }

            bool ok = CFrontPanel::instance()->setLED(properties);
            success.success = ok;
            return Core::ERROR_NONE;
        }

        void FrontPanelImplementation::setBlink(const JsonObject& blinkInfo)
        {
            CFrontPanel::instance()->setBlink(blinkInfo);
        }

        Core::hresult FrontPanelImplementation::SetBlink(const string& blinkInfo, FrontPanelSuccess& success)
        {
            LOGINFO("SetBlink: %s", blinkInfo.c_str());
            bool ok = false;
            try {
                JsonObject inputObj;
                inputObj.FromString(blinkInfo);
                setBlink(inputObj);
                ok = true;
            } catch (...) {
                LOGERR("Exception during SetBlink");
                ok = false;
            }
            success.success = ok;
            return Core::ERROR_NONE;
        }

        // ── DeviceSettingsClientHelper lifecycle callbacks ──────────────────────

        void FrontPanelImplementation::OnDeviceSettingsActivated()
        {
            LOGINFO("OnDeviceSettingsActivated: setting FPD acquirer and updating config from DSHelper");

            // Give CFrontPanel a lambda to acquire IDeviceSettingsFPD on demand
            CFrontPanel::instance()->setFPDAcquirer([this]() {
                return DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsFPD>();
            });
            LOGINFO("OnDeviceSettingsActivated: FPD acquirer set; config accessible via DSHelper "
                    "(indicators=%zu colors=%zu textDisplays=%zu bindings=%zu)",
                    DSHelper::getFPDIndicators().size(),  DSHelper::getFPDColors().size(),
                    DSHelper::getFPDTextDisplays().size(), DSHelper::getFPDColorBindings().size());

            // Register for FPD notifications
            auto* fpd = DSHelper::AcquireSubInterface<Exchange::IDeviceSettingsFPD>();
            if (fpd) {
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
