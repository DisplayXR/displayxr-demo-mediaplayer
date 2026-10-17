// SPDX-License-Identifier: Apache-2.0
//
// InputProviderClient — which input provider holds this app's controllers / hands, and
// asking for another, over XR_DXR_input_providers (spec 1, ADR-034 Amendment 5).
//
// The runtime lists its resident providers with their OWN display names; the player shows
// those strings verbatim and never recognises an id or a name. A pick is sent as this
// app's non-persisted request by default; only the user's explicit "remember for this
// app" turns on persist, which writes the same per-app pin the DisplayXR Dashboard writes.
//
// Same shape as LiftClient: a pure state machine over a table of entry points (Api), so
// tests/input_provider_client_test.cpp drives it through fakes without a runtime.
//
// Note for this player: roles move on the app's next xrSyncActions, and the media player
// declares no actions, so its own holders do not move on a request. The request is still
// recorded (requestedFamilies) and a persisted pin applies to the app's executable.
#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_input_providers.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mp::input {

struct Api {
    PFN_xrEnumerateInputProvidersDXR enumerate = nullptr;
    PFN_xrSetInputProviderPreferenceDXR setPreference = nullptr;
    bool Complete() const { return enumerate && setPreference; }
};

// One resident provider as THIS app sees it (strings verbatim from the runtime).
struct Provider {
    std::string id;
    std::string name;
    XrInputRoleFamilyFlagsDXR supported = 0;
    XrInputRoleFamilyFlagsDXR current = 0;    // this app's roles resolve to it now
    XrInputRoleFamilyFlagsDXR requested = 0;  // this app's non-persisted request names it
    bool present = false;
    bool userDefault = false;
    bool appPinned = false;
};

XrInputRoleFamilyFlagsDXR FamilyBit(XrInputRoleFamilyDXR family);
const char* FamilyKey(XrInputRoleFamilyDXR family);  // "controllers" | "hand_tracking" | "gamepad"
const char* FamilyLabel(XrInputRoleFamilyDXR family);  // "Controllers" | "Hands" | "Gamepad"
bool ParseFamily(const std::string& key, XrInputRoleFamilyDXR& out);

class InputProviderClient {
public:
    InputProviderClient() = default;
    InputProviderClient(const InputProviderClient&) = delete;
    InputProviderClient& operator=(const InputProviderClient&) = delete;

    // Bind once the session exists (the setter needs it; enumeration needs instance +
    // system). An incomplete api leaves the client unattached: the row stays hidden.
    // `::XrSession`: mp::XrSession (the app's session class) shadows the handle type.
    void Attach(const Api& api, XrInstance instance, XrSystemId system, ::XrSession session);
    void Detach();
    bool Attached() const { return api_.Complete() && instance_ != XR_NULL_HANDLE; }

    // Re-enumerate (two-call idiom). Logs the list when it changes.
    void Refresh();
    // Feed every XrEventDataBuffer; true if it was the providers-changed event (refreshes).
    bool OnEvent(const XrEventDataBaseHeader* ev);

    const std::vector<Provider>& Providers() const { return providers_; }
    // The row is drawn only when the runtime lists at least one provider.
    bool Visible() const { return Attached() && !providers_.empty(); }
    // Some listed provider can supply `family`.
    bool FamilyOffered(XrInputRoleFamilyDXR family) const;
    // Index of the provider holding `family` for this app now (isCurrent), -1 = none.
    int Current(XrInputRoleFamilyDXR family) const;
    // Index of the provider this app's request names for `family`, -1 = none.
    int Requested(XrInputRoleFamilyDXR family) const;

    // Ask for `providerId` ("" = back to the default) for one family. persist only on an
    // explicit user action. Returns "" on success, else why not. Re-enumerates after.
    std::string SetPreference(XrInputRoleFamilyDXR family, const std::string& providerId,
                              bool persist);

private:
    Api api_{};
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    ::XrSession session_ = XR_NULL_HANDLE;
    std::vector<Provider> providers_;
};

}  // namespace mp::input
