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

#include <interfaces/IFrontPanel.h>
#include <interfaces/json/JFrontPanel.h>
#include <interfaces/json/JsonData_FrontPanel.h>

using namespace WPEFramework;


namespace WPEFramework {

    namespace Plugin {
        class FrontPanel : public PluginHost::IPlugin, public PluginHost::JSONRPC {
        public:
                // We do not allow this plugin to be copied !!
                FrontPanel(const FrontPanel&) = delete;
                FrontPanel& operator=(const FrontPanel&) = delete;

                FrontPanel()
                : PluginHost::IPlugin()
                , PluginHost::JSONRPC()
                , _service(nullptr)
                , _frontPanel(nullptr)
                , _connectionId(0)
                {

                }
                virtual ~FrontPanel()
                {

                }

                BEGIN_INTERFACE_MAP(FrontPanel)
                INTERFACE_ENTRY(PluginHost::IPlugin)
                INTERFACE_ENTRY(PluginHost::IDispatcher)
                INTERFACE_AGGREGATE(Exchange::IFrontPanel, _frontPanel)
                END_INTERFACE_MAP

                //  IPlugin methods
                // -------------------------------------------------------------------------------------------------------
		        const string Initialize(PluginHost::IShell* service) override;
                void Deinitialize(PluginHost::IShell* service) override;
                string Information() const override;
                //Begin methods

            private:
                void Deactivated(RPC::IRemoteConnection* connection);

            private:
                PluginHost::IShell* _service{};
                Exchange::IFrontPanel* _frontPanel;
                uint32_t _connectionId;

        };
        

	} // namespace Plugin
} // namespace WPEFramework
