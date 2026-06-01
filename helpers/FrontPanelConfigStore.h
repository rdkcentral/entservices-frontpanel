/*
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2026 RDK Management
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
*/

#pragma once

#include <unordered_map>
#include <vector>

#include <interfaces/IDeviceSettingsFPD.h>

namespace WPEFramework {
namespace Plugin {

struct FrontPanelConfigStore {
    std::vector<Exchange::IDeviceSettingsFPD::FPDIndicator> indicators;
    std::vector<Exchange::IDeviceSettingsFPD::dsFPDIndicatorConfig_t> indicatorConfigs;
    std::vector<Exchange::IDeviceSettingsFPD::dsFPDTextDisplayConfig_t> textDisplayConfigs;
    std::unordered_map<int32_t, uint32_t> colorValueById;
    std::unordered_map<int32_t, std::vector<uint32_t>> colorsByIndicatorId;
    std::unordered_map<int32_t, std::vector<uint32_t>> colorsByTextDisplayId;

    void Clear();
};

bool LoadFrontPanelConfig(Exchange::IDeviceSettingsFPD* deviceSettingsFPD, FrontPanelConfigStore& configStore);

} // namespace Plugin
} // namespace WPEFramework
