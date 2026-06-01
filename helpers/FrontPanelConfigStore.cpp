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

#include "FrontPanelConfigStore.h"

namespace WPEFramework {
namespace Plugin {

void FrontPanelConfigStore::Clear()
{
    indicators.clear();
    indicatorConfigs.clear();
    textDisplayConfigs.clear();
    colorValueById.clear();
    colorsByIndicatorId.clear();
    colorsByTextDisplayId.clear();
}

bool LoadFrontPanelConfig(Exchange::IDeviceSettingsFPD* deviceSettingsFPD, FrontPanelConfigStore& configStore)
{
    configStore.Clear();

    if (deviceSettingsFPD == nullptr) {
        return false;
    }

    Exchange::IDeviceSettingsFPD::IFPDTextDisplayConfigIterator* textDisplays = nullptr;
    Exchange::IDeviceSettingsFPD::IFPDIndicatorConfigIterator* indicators = nullptr;
    Exchange::IDeviceSettingsFPD::IFPDColorConfigIterator* colors = nullptr;
    Exchange::IDeviceSettingsFPD::IFPDColorBindingIterator* colorBindings = nullptr;

    const Core::hresult result = deviceSettingsFPD->GetFrontPanelConfig(textDisplays, indicators, colors, colorBindings);
    if (result != Core::ERROR_NONE) {
        if (textDisplays != nullptr) {
            textDisplays->Release();
        }
        if (indicators != nullptr) {
            indicators->Release();
        }
        if (colors != nullptr) {
            colors->Release();
        }
        if (colorBindings != nullptr) {
            colorBindings->Release();
        }
        return false;
    }

    if (colors != nullptr) {
        Exchange::IDeviceSettingsFPD::dsFPDColorConfig_t colorConfig;
        while (colors->Next(colorConfig) == true) {
            configStore.colorValueById[colorConfig.id] = colorConfig.color;
        }
        colors->Release();
    }

    if (colorBindings != nullptr) {
        Exchange::IDeviceSettingsFPD::dsFPDColorBinding_t binding;
        while (colorBindings->Next(binding) == true) {
            const auto colorIt = configStore.colorValueById.find(binding.colorId);
            if (colorIt == configStore.colorValueById.end()) {
                continue;
            }

            if (binding.targetType == Exchange::IDeviceSettingsFPD::DS_FPD_COLOR_TARGET_INDICATOR) {
                configStore.colorsByIndicatorId[binding.targetId].push_back(colorIt->second);
            } else if (binding.targetType == Exchange::IDeviceSettingsFPD::DS_FPD_COLOR_TARGET_TEXTDISPLAY) {
                configStore.colorsByTextDisplayId[binding.targetId].push_back(colorIt->second);
            }
        }
        colorBindings->Release();
    }

    if (indicators != nullptr) {
        Exchange::IDeviceSettingsFPD::dsFPDIndicatorConfig_t indicatorConfig;
        while (indicators->Next(indicatorConfig) == true) {
            configStore.indicatorConfigs.push_back(indicatorConfig);
            if ((indicatorConfig.id >= Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MESSAGE) &&
                (indicatorConfig.id < Exchange::IDeviceSettingsFPD::DS_FPD_INDICATOR_MAX)) {
                configStore.indicators.push_back(static_cast<Exchange::IDeviceSettingsFPD::FPDIndicator>(indicatorConfig.id));
            }
        }
        indicators->Release();
    }

    if (textDisplays != nullptr) {
        Exchange::IDeviceSettingsFPD::dsFPDTextDisplayConfig_t textDisplayConfig;
        while (textDisplays->Next(textDisplayConfig) == true) {
            configStore.textDisplayConfigs.push_back(textDisplayConfig);
        }
        textDisplays->Release();
    }

    return true;
}

} // namespace Plugin
} // namespace WPEFramework
