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

#include "FrontPanel.h"
#include <algorithm>


#include "libIBus.h"

#include "UtilsJsonRpc.h"
#include "UtilsIarm.h"

#define API_VERSION_NUMBER_MAJOR 1
#define API_VERSION_NUMBER_MINOR 0
#define API_VERSION_NUMBER_PATCH 6

namespace WPEFramework
{
    namespace {

        static Plugin::Metadata<Plugin::FrontPanel> metadata(
            // Version (Major, Minor, Patch)
            API_VERSION_NUMBER_MAJOR, API_VERSION_NUMBER_MINOR, API_VERSION_NUMBER_PATCH,
            // Preconditions
            {},
            // Terminations
            {},
            // Controls
            {}
        );
    }
    
    namespace Plugin
    {

        const string FrontPanel::Initialize(PluginHost::IShell *service)
        {
           SYSLOG(Logging::Startup, (string(_T("FrontPanel::Initialize"))));

           string msg = "";

           ASSERT(nullptr != service);
           ASSERT(nullptr == _service);
           ASSERT(nullptr == _frontPanel);
           ASSERT(0 == _connectionId);


           _service = service;
           _service->AddRef();
           _frontPanel = _service->Root<Exchange::IFrontPanel>(_connectionId, 5000, _T("FrontPanelImplementation"));

           if(nullptr != _frontPanel)
            {
                _frontPanel->Configure(service);
                Exchange::JFrontPanel::Register(*this, _frontPanel);
                LOGINFO("HdmiCecSource plugin is available. Successfully activated FrontPanel Plugin");
            }
            else
            {
                msg = "FrontPanel plugin is not available";
                LOGINFO("FrontPanel plugin is not available. Failed to activate FrontPanel Plugin");
            }

           // On success return empty, to indicate there is no error text.
           return msg;
        }

        void FrontPanel::Deinitialize(PluginHost::IShell* service)
        {
           SYSLOG(Logging::Shutdown, (string(_T("FrontPanel::Deinitialize"))));

           ASSERT(nullptr != service);

           if(nullptr != _frontPanel)
           {
             Exchange::JFrontPanel::Unregister(*this);
             _frontPanel->Release();
             _frontPanel = nullptr;

             RPC::IRemoteConnection* connection = _service->RemoteConnection(_connectionId);
             if (connection != nullptr)
             {
                try{
                    connection->Terminate();
                }
                catch(const std::exception& e)
                {
                    std::string errorMessage = "Failed to terminate connection: ";
                    errorMessage += e.what();
                    LOGWARN("%s",errorMessage.c_str());
                }

                connection->Release();
             }
           }

           _connectionId = 0;
           _service->Release();
           _service = nullptr;
           LOGINFO("FrontPanel plugin is deactivated. Successfully deactivated FrontPanel Plugin");
        }

        string FrontPanel::Information() const
        {
            return("This FrontPanel PLugin Facilitates the use of the front panel LEDs on the device. It allows to set the brightness of the LEDs, turn them on/off, set the clock brightness, and get information about the front panel lights.");
        }

        void FrontPanel::Deactivated(RPC::IRemoteConnection* connection)
        {
            if (connection->Id() == _connectionId)
            {
                ASSERT(_service != nullptr);
                Core::IWorkerPool::Instance().Submit(PluginHost::IShell::Job::Create(_service, PluginHost::IShell::DEACTIVATED, PluginHost::IShell::FAILURE));
            }
        }





    } // namespace Plugin
} // namespace WPEFramework
