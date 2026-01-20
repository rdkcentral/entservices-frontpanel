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

#include <mutex>
#include "Module.h"
#include "libIARM.h"
#include "frontpanel.h"
#include <interfaces/IPowerManager.h>
#include "PowerManagerInterface.h"
#include <interfaces/IFrontPanel.h>

using namespace WPEFramework;
using PowerState = WPEFramework::Exchange::IPowerManager::PowerState;
using ThermalTemperature = WPEFramework::Exchange::IPowerManager::ThermalTemperature;

#define DATA_LED  "data_led"
#define RECORD_LED "record_led"


namespace WPEFramework {

    namespace Plugin {


		// This is a server for a JSONRPC communication channel.
		// For a plugin to be capable to handle JSONRPC, inherit from PluginHost::JSONRPC.
		// By inheriting from this class, the plugin realizes the interface PluginHost::IDispatcher.
		// This realization of this interface implements, by default, the following methods on this plugin
		// - exists
		// - register
		// - unregister
		// Any other methood to be handled by this plugin  can be added can be added by using the
		// templated methods Register on the PluginHost::JSONRPC class.
		// As the registration/unregistration of notifications is realized by the class PluginHost::JSONRPC,
		// this class exposes a public method called, Notify(), using this methods, all subscribed clients
		// will receive a JSONRPC message as a notification, in case this method is called.
        class FrontPanelImplementation : public Exchange::IFrontPanel {
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
            Core::hresult GetFrontPanelLights(IFrontPanelLightsListIterator*& supportedLights , string &supportedLightsInfo, bool &success) override;
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

            BEGIN_INTERFACE_MAP(FrontPanelImplementation)
                INTERFACE_ENTRY(Exchange::IFrontPanel)
            END_INTERFACE_MAP
        public:
            static FrontPanelImplementation* _instance;
        private:
            static int m_LedDisplayPatternUpdateTimerInterval;

            bool           m_runUpdateTimer;
            std::mutex      m_updateTimerMutex;
            PowerManagerInterfaceRef _powerManagerPlugin;
            Core::Sink<PowerManagerNotification> _pwrMgrNotification;
            bool _registeredEventHandlers;

        };

	} // namespace Plugin
} // namespace WPEFramework
