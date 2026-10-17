// SPDX-License-Identifier: Apache-2.0
//
// InputProviderClient — see InputProviderClient.h. No loader, no provider names.

#include "media/InputProviderClient.h"

#include "Log.h"

#include <cstdio>
#include <cstring>

namespace mp::input {

XrInputRoleFamilyFlagsDXR FamilyBit(XrInputRoleFamilyDXR family) {
    switch (family) {
        case XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR: return XR_INPUT_ROLE_FAMILY_CONTROLLERS_BIT_DXR;
        case XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR:
            return XR_INPUT_ROLE_FAMILY_HAND_TRACKING_BIT_DXR;
        case XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR: return XR_INPUT_ROLE_FAMILY_GAMEPAD_BIT_DXR;
        default: return 0;
    }
}

const char* FamilyKey(XrInputRoleFamilyDXR family) {
    switch (family) {
        case XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR: return "controllers";
        case XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR: return "hand_tracking";
        case XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR: return "gamepad";
        default: return "unknown";
    }
}

const char* FamilyLabel(XrInputRoleFamilyDXR family) {
    switch (family) {
        case XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR: return "Controllers";
        case XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR: return "Hands";
        case XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR: return "Gamepad";
        default: return "?";
    }
}

bool ParseFamily(const std::string& key, XrInputRoleFamilyDXR& out) {
    if (key == "controllers")
        out = XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR;
    else if (key == "hand_tracking" || key == "hands")
        out = XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR;
    else if (key == "gamepad")
        out = XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR;
    else
        return false;
    return true;
}

void InputProviderClient::Attach(const Api& api, XrInstance instance, XrSystemId system,
                                 ::XrSession session) {
    Detach();
    api_ = api;
    instance_ = instance;
    system_ = system;
    session_ = session;
    if (!Attached()) return;
    Refresh();
}

void InputProviderClient::Detach() {
    api_ = Api{};
    instance_ = XR_NULL_HANDLE;
    system_ = XR_NULL_SYSTEM_ID;
    session_ = XR_NULL_HANDLE;
    providers_.clear();
}

void InputProviderClient::Refresh() {
    if (!Attached()) return;
    for (int attempt = 0; attempt < 2; ++attempt) {
        uint32_t n = 0;
        XrResult r = api_.enumerate(instance_, system_, 0, &n, nullptr);
        if (XR_FAILED(r)) {
            LOG_WARN("xrEnumerateInputProvidersDXR failed: XrResult=%d", (int)r);
            providers_.clear();
            return;
        }
        std::vector<XrInputProviderPropertiesDXR> raw(n);
        for (auto& p : raw) {
            p = XrInputProviderPropertiesDXR{};
            p.type = XR_TYPE_INPUT_PROVIDER_PROPERTIES_DXR;
        }
        if (n > 0) {
            r = api_.enumerate(instance_, system_, n, &n, raw.data());
            if (r == XR_ERROR_SIZE_INSUFFICIENT) continue;  // the set grew in between
            if (XR_FAILED(r)) {
                LOG_WARN("xrEnumerateInputProvidersDXR failed: XrResult=%d", (int)r);
                providers_.clear();
                return;
            }
        }
        std::vector<Provider> list;
        for (uint32_t i = 0; i < n && i < raw.size(); ++i) {
            Provider p;
            p.id.assign(raw[i].providerId, strnlen(raw[i].providerId, sizeof(raw[i].providerId)));
            p.name.assign(raw[i].displayName,
                          strnlen(raw[i].displayName, sizeof(raw[i].displayName)));
            if (p.id.empty()) continue;
            if (p.name.empty()) p.name = p.id;
            p.supported = raw[i].supportedFamilies;
            p.current = raw[i].currentFamilies;
            p.requested = raw[i].requestedFamilies;
            p.present = raw[i].isPresent == XR_TRUE;
            p.userDefault = raw[i].isUserDefault == XR_TRUE;
            p.appPinned = raw[i].isAppPinned == XR_TRUE;
            list.push_back(std::move(p));
        }
        bool changed = list.size() != providers_.size();
        for (size_t i = 0; !changed && i < list.size(); ++i) {
            const Provider &a = list[i], &b = providers_[i];
            changed = a.id != b.id || a.current != b.current || a.requested != b.requested ||
                      a.present != b.present || a.appPinned != b.appPinned ||
                      a.userDefault != b.userDefault || a.supported != b.supported;
        }
        providers_ = std::move(list);
        if (changed) {
            LOG_INFO("Input providers: %zu listed", providers_.size());
            for (const Provider& p : providers_)
                LOG_INFO(
                    "  input provider '%s' (id %s): supplies 0x%llx, holds 0x%llx, "
                    "requested 0x%llx%s%s%s",
                    p.name.c_str(), p.id.c_str(), (unsigned long long)p.supported,
                    (unsigned long long)p.current, (unsigned long long)p.requested,
                    p.present ? "" : ", absent", p.userDefault ? ", user default" : "",
                    p.appPinned ? ", pinned for this app" : "");
        }
        return;
    }
}

bool InputProviderClient::OnEvent(const XrEventDataBaseHeader* ev) {
    if (!ev || ev->type != XR_TYPE_EVENT_DATA_INPUT_PROVIDERS_CHANGED_DXR) return false;
    Refresh();  // instance-scoped: no session to match
    return true;
}

bool InputProviderClient::FamilyOffered(XrInputRoleFamilyDXR family) const {
    const XrInputRoleFamilyFlagsDXR bit = FamilyBit(family);
    for (const Provider& p : providers_)
        if (p.supported & bit) return true;
    return false;
}

int InputProviderClient::Current(XrInputRoleFamilyDXR family) const {
    const XrInputRoleFamilyFlagsDXR bit = FamilyBit(family);
    for (size_t i = 0; i < providers_.size(); ++i)
        if (providers_[i].current & bit) return (int)i;
    return -1;
}

int InputProviderClient::Requested(XrInputRoleFamilyDXR family) const {
    const XrInputRoleFamilyFlagsDXR bit = FamilyBit(family);
    for (size_t i = 0; i < providers_.size(); ++i)
        if (providers_[i].requested & bit) return (int)i;
    return -1;
}

std::string InputProviderClient::SetPreference(XrInputRoleFamilyDXR family,
                                               const std::string& providerId, bool persist) {
    if (!Attached() || session_ == XR_NULL_HANDLE) return "input provider choice is not available";
    if (FamilyBit(family) == 0) return "unknown input family";
    if (providerId.size() >= XR_MAX_INPUT_PROVIDER_ID_SIZE_DXR) return "provider id too long";
    XrInputProviderPreferenceInfoDXR info{};
    info.type = XR_TYPE_INPUT_PROVIDER_PREFERENCE_INFO_DXR;
    info.family = family;
    std::snprintf(info.providerId, sizeof(info.providerId), "%s", providerId.c_str());
    info.persist = persist ? XR_TRUE : XR_FALSE;
    const XrResult r = api_.setPreference(session_, &info);
    if (XR_FAILED(r)) {
        LOG_WARN("xrSetInputProviderPreferenceDXR(%s, %s, persist=%d) failed: XrResult=%d",
                 FamilyKey(family), providerId.empty() ? "<default>" : "<id>", persist ? 1 : 0,
                 (int)r);
        return r == XR_ERROR_FEATURE_UNSUPPORTED
                   ? (persist ? std::string("this app's choice cannot be remembered here")
                              : std::string("input provider choice is not supported here"))
                   : std::string("the input provider request failed");
    }
    LOG_INFO("Input provider request: %s -> %s%s", FamilyKey(family),
             providerId.empty() ? "default" : providerId.c_str(), persist ? " (remembered)" : "");
    Refresh();  // a no-op set raises no event; read the recorded request back now
    return std::string();
}

}  // namespace mp::input
