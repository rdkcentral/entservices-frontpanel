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

#pragma once

/**
 * @file DS_COMRPC/FrontPanelImplementation.h
 *
 * @brief COM-RPC path for the FrontPanel plugin.
 *
 * Compiled when USE_DEVICESETTING_PLUGIN is defined.
 * Connects to entservices-devicesettings via COM-RPC (IDeviceSettingsFPD)
 * using DSHelper (DeviceSettingsInterface.h — DeviceSettingsClientHelper.h removed).
 * Does NOT link libds.so or libdshal-cli.so.
 */

#include <mutex>
#include "Module.h"
#include "frontpanel.h"
#include <interfaces/IPowerManager.h>
#include "PowerManagerInterface.h"
#include <interfaces/IFrontPanel.h>
#include "DeviceSettingsInterface.h"
#include <interfaces/IDeviceSettingsFPD.h>

using namespace WPEFramework;
using PowerState = WPEFramework::Exchange::IPowerManager::PowerState;
using ThermalTemperature = WPEFramework::Exchange::IPowerManager::ThermalTemperature;

#define DATA_LED  "data_led"
#define RECORD_LED "record_led"

namespace WPEFramework {

    namespace Plugin {

        class FrontPanelImplementation
            : public Exchange::IFrontPanel
            , public DSHelper  // DSHelper is the renamed DeviceSettingsClientHelper
        {
        private:
            class PowerManagerNotification : public Exchange::IPowerManager::IModeChangedNotification {
            private:
                PowerManagerNotification(const PowerManagerNotification&) = delete;
                PowerManagerNotification& operator=(const PowerManagerNotification&) = delete;

            public:
                explicit PowerManagerNotification(FrontPanelImplementation& parent)
                    : _parent(parent)
                {
                }
                ~PowerManagerNotification() override = default;

            public:
                void OnPowerModeChanged(const PowerState currentState, const PowerState newState) override
                {
                    _parent.onPowerModeChanged(currentState, newState);
                }

                template <typename T>
                T* baseInterface()
                {
                    static_assert(std::is_base_of<T, PowerManagerNotification>(), "base type mismatch");
                    return static_cast<T*>(this);
                }

                BEGIN_INTERFACE_MAP(PowerManagerNotification)
                INTERFACE_ENTRY(Exchange::IPowerManager::IModeChangedNotification)
                END_INTERFACE_MAP

            private:
                FrontPanelImplementation& _parent;
            };

            // Inner delegate for IDeviceSettingsFPD notifications
            class DSFPDNotification : public Exchange::IDeviceSettingsFPD::INotification {
            private:
                DSFPDNotification(const DSFPDNotification&) = delete;
                DSFPDNotification& operator=(const DSFPDNotification&) = delete;
            public:
                explicit DSFPDNotification(FrontPanelImplementation& parent) : _parent(parent) {}
                ~DSFPDNotification() override = default;
                // Extend here when IDeviceSettingsFPD::INotification grows new events.
                void OnFPDTimeFormatChanged(const Exchange::IDeviceSettingsFPD::FPDTimeFormat) override {}

                BEGIN_INTERFACE_MAP(DSFPDNotification)
                    INTERFACE_ENTRY(Exchange::IDeviceSettingsFPD::INotification)
                END_INTERFACE_MAP
            private:
                FrontPanelImplementation& _parent;
            };

            // We do not allow this plugin to be copied !!
            FrontPanelImplementation(const FrontPanelImplementation&) = delete;
            FrontPanelImplementation& operator=(const FrontPanelImplementation&) = delete;

            std::vector<string> getFrontPanelLights();
            JsonObject getFrontPanelLightsInfo();
            void setBlink(const JsonObject& blinkInfo);
            void InitializePowerManager(PluginHost::IShell *service);

            //Begin methods
            Core::hresult SetBrightness(const string& index, const uint32_t brightness, FrontPanelSuccess& success) override;
            Core::hresult GetBrightness(const string& index, uint32_t& brightness, bool& success) override;
            Core::hresult PowerLedOn(const string& index, FrontPanelSuccess& success) override;
            Core::hresult PowerLedOff(const string& index, FrontPanelSuccess& success) override;
            Core::hresult GetFrontPanelLights(IFrontPanelLightsListIterator*& supportedLights, string& supportedLightsInfo, bool& success) override;
            Core::hresult SetLED(const string& ledIndicator, const uint32_t brightness, const string& color, const uint32_t red, const uint32_t green, const uint32_t blue, FrontPanelSuccess& success) override;
            Core::hresult SetBlink(const string& blinkInfo, FrontPanelSuccess& success) override;
            Core::hresult Configure(PluginHost::IShell* service) override;
            //End methods

        public:
            FrontPanelImplementation();
            virtual ~FrontPanelImplementation();
            void onPowerModeChanged(const PowerState currentState, const PowerState newState);
            void updateLedTextPattern();
            void registerEventHandlers();

        protected:
            /**
             * Called when DeviceSettings plugin (re-)activates.
             * Loads FPD config from IDeviceSettingsFPD and re-registers
             * the DS notification delegate.
             */
            void OnDeviceSettingsActivated() override;
            /**
             * Called when DeviceSettings plugin deactivates.
             * Clears the cached FPD interface from CFrontPanel.
             * Do NOT call AcquireSubInterface here — the link is already down.
             */
            void OnDeviceSettingsDeactivated() override;

        public:
            BEGIN_INTERFACE_MAP(FrontPanelImplementation)
                INTERFACE_ENTRY(Exchange::IFrontPanel)
            END_INTERFACE_MAP

        public:
            static FrontPanelImplementation* _instance;

        private:
            static int m_LedDisplayPatternUpdateTimerInterval;

            bool            m_runUpdateTimer;
            std::mutex      m_updateTimerMutex;
            PowerManagerInterfaceRef _powerManagerPlugin;
            Core::Sink<PowerManagerNotification> _pwrMgrNotification;
            bool _registeredEventHandlers;
            // COM-RPC FPD notification sink — must be last in init order
            Core::Sink<DSFPDNotification> _dsFpdNotification;
        };

    } // namespace Plugin
} // namespace WPEFramework
