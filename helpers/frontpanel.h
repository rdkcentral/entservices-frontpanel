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
* @defgroup include
* @{
**/


#ifndef FRONTPANEL_H
#define FRONTPANEL_H

#include <string>
#include <list>
#include <vector>

#include <plugins/plugins.h>
#include <functional>

#ifdef USE_DEVICESETTING_PLUGIN
#include "DeviceSettingsConfig.h"
#endif

namespace WPEFramework
{

    namespace Plugin
    {

        class FrontPanelImplementation;
        class CFrontPanel;

        class BlinkInfo
        {
        private:
            BlinkInfo() = delete;
            BlinkInfo& operator=(const BlinkInfo& RHS) = delete;

        public:
            BlinkInfo(CFrontPanel* fp)
            : m_frontPanel(fp)
            {
            }
            BlinkInfo(const BlinkInfo& copy)
            : m_frontPanel(copy.m_frontPanel)
            {
            }
            ~BlinkInfo() {}

            inline bool operator==(const BlinkInfo& RHS) const
            {
                return(m_frontPanel == RHS.m_frontPanel);
            }

        public:
            uint64_t Timed(const uint64_t scheduledTime);

        private:
            CFrontPanel* m_frontPanel;
        };


        typedef struct _FrontPanelBlinkInfo
        {
            std::string ledIndicator;
            std::string colorName;
            unsigned int colorValue;
            int brightness;
            int durationInMs;
            int colorMode;
        } FrontPanelBlinkInfo;

        typedef enum _frontPanelIndicator
        {
            FRONT_PANEL_INDICATOR_CLOCK,
            FRONT_PANEL_INDICATOR_MESSAGE,
            FRONT_PANEL_INDICATOR_POWER,
            FRONT_PANEL_INDICATOR_RECORD,
            FRONT_PANEL_INDICATOR_REMOTE,
            FRONT_PANEL_INDICATOR_RFBYPASS,
            FRONT_PANEL_INDICATOR_ALL
        } frontPanelIndicator;

        class CFrontPanel
        {
        public:
            static CFrontPanel* instance(PluginHost::IShell *service = nullptr);
            static void deinitialize();
            bool start();
            bool stop();
            std::string getLastError();
            void addEventObserver(FrontPanelImplementation* o);
            void removeEventObserver(FrontPanelImplementation* o);
            bool setBrightness(int fp_brighness);
            int getBrightness();
            bool powerOffLed(frontPanelIndicator fp_indicator);
            bool powerOnLed(frontPanelIndicator fp_indicator);
            bool powerOffAllLed();
            bool powerOnAllLed();
            void setPowerStatus(bool powerStatus);
            bool setLED(const JsonObject& blinkInfo);
            void setBlink(const JsonObject& blinkInfo);
            void loadPreferences();
            void stopBlinkTimer();

            void onBlinkTimer();
            static int initDone;

            // ── Per-indicator brightness (not in original CFrontPanel API)
            bool setBrightnessByName(const std::string& iarmName, int brightness);
            int  getBrightnessByName(const std::string& iarmName);

            // ── Enumeration / info helpers (delegated from FrontPanelImplementation)
            std::vector<std::string> getFrontPanelLights();
            JsonObject               getFrontPanelLightsInfo();

#ifdef USE_DEVICESETTING_PLUGIN
            /**
             * Provide a factory that yields an AddRef'd IDeviceSettingsFPD* on demand.
             * CFrontPanel calls it per operation and Release()s immediately.
             * Pass nullptr (or empty function) to disable the DS path.
             */
            void setFPDAcquirer(std::function<Exchange::IDeviceSettingsFPD*()> acquirer);

            /**
             * Load the front-panel config from the live DS interface into the
             * internal FrontPanelConfigStore.  Call from OnDeviceSettingsActivated.
             */
            void updateFPDConfigStore(Exchange::IDeviceSettingsFPD* fpd);

            /** Drop both the acquirer and the config store. */
            void clearFPDInterface();
#endif

        private:
            CFrontPanel();
            static CFrontPanel* s_instance;
            void startBlinkTimer(int numberOfBlinkRepeats);
            void setBlinkLed(FrontPanelBlinkInfo blinkInfo);
            JsonObject m_preferencesHash;  // is this needed

            BlinkInfo m_blinkTimer;
            bool m_isBlinking;
            std::vector<FrontPanelBlinkInfo> m_blinkList;
            std::list<FrontPanelImplementation*> observers_;

            std::string lastError_;

#ifdef USE_DEVICESETTING_PLUGIN
            /** Per-operation acquirer for IDeviceSettingsFPD (set from FrontPanelImplementation). */
            std::function<Exchange::IDeviceSettingsFPD*()> m_fpdAcquirer;
            /** Cached front-panel config (indicators, colors, bindings). */
            FrontPanelConfigStore m_fpConfigStore;
#endif
        };
    } // namespace Plugin
} // namespace WPEFramework


#endif


/** @} */
/** @} */
