// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for InputProviderClient (XR_DXR_input_providers spec 1) over a fake
// entry-point table: hidden when absent / empty, providers listed verbatim, the current
// holder per family, the request (non-persisted by default, persist only when asked),
// event-driven refresh, and refused requests. No runtime.

#include "media/InputProviderClient.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mp::input;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                            \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

XrInstance const kInstance = (XrInstance)(uintptr_t)0x1257;
XrSystemId const kSystem = 7;
XrSession const kSession = (XrSession)(uintptr_t)0x5e55;

struct Fake {
    std::vector<XrInputProviderPropertiesDXR> providers;
    XrResult setResult = XR_SUCCESS;
    int enumCalls = 0, setCalls = 0;
    XrInputProviderPreferenceInfoDXR lastSet{};
    bool applyRequests = true;  // the fake records requests into requestedFamilies
};
Fake g;

XrInputProviderPropertiesDXR MakeProvider(const char* id, const char* name,
                                          XrInputRoleFamilyFlagsDXR supported,
                                          XrInputRoleFamilyFlagsDXR current, bool present = true) {
    XrInputProviderPropertiesDXR p{};
    p.type = XR_TYPE_INPUT_PROVIDER_PROPERTIES_DXR;
    std::snprintf(p.providerId, sizeof(p.providerId), "%s", id);
    std::snprintf(p.displayName, sizeof(p.displayName), "%s", name);
    p.supportedFamilies = supported;
    p.currentFamilies = current;
    p.isPresent = present ? XR_TRUE : XR_FALSE;
    return p;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeEnumerate(XrInstance inst, XrSystemId sys, uint32_t cap,
                                             uint32_t* count, XrInputProviderPropertiesDXR* out) {
    ++g.enumCalls;
    if (inst != kInstance || sys != kSystem) return XR_ERROR_HANDLE_INVALID;
    *count = (uint32_t)g.providers.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < g.providers.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    for (size_t i = 0; i < g.providers.size(); ++i) {
        if (out[i].type != XR_TYPE_INPUT_PROVIDER_PROPERTIES_DXR)
            return XR_ERROR_VALIDATION_FAILURE;
        out[i] = g.providers[i];
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL FakeSet(XrSession s, const XrInputProviderPreferenceInfoDXR* info) {
    ++g.setCalls;
    if (s != kSession) return XR_ERROR_HANDLE_INVALID;
    g.lastSet = *info;
    if (XR_FAILED(g.setResult)) return g.setResult;
    if (g.applyRequests) {
        const XrInputRoleFamilyFlagsDXR bit = FamilyBit(info->family);
        for (auto& p : g.providers) {
            p.requestedFamilies &= ~bit;
            if (info->providerId[0] && std::strcmp(p.providerId, info->providerId) == 0)
                p.requestedFamilies |= bit;
        }
    }
    return XR_SUCCESS;
}

Api FakeApi() {
    Api a;
    a.enumerate = FakeEnumerate;
    a.setPreference = FakeSet;
    return a;
}

const XrInputRoleFamilyFlagsDXR kCtl = XR_INPUT_ROLE_FAMILY_CONTROLLERS_BIT_DXR;
const XrInputRoleFamilyFlagsDXR kHands = XR_INPUT_ROLE_FAMILY_HAND_TRACKING_BIT_DXR;

void TestHidden() {
    g = Fake{};
    InputProviderClient c;
    CHECK(!c.Visible(), "unattached -> hidden");
    Api partial = FakeApi();
    partial.setPreference = nullptr;  // the extension's entry points did not resolve
    c.Attach(partial, kInstance, kSystem, kSession);
    CHECK(!c.Attached() && !c.Visible() && g.enumCalls == 0, "incomplete api -> hidden, no calls");
    c.Attach(FakeApi(), kInstance, kSystem, kSession);
    CHECK(c.Attached() && !c.Visible(), "zero providers listed -> hidden (not an error)");
}

void TestListAndCurrent() {
    g = Fake{};
    g.providers = {MakeProvider("p.hands", "Provider H", kHands, kHands),
                   MakeProvider("p.ctl", "Provider C", kCtl, kCtl),
                   MakeProvider("emulated", "Emulated controllers", kCtl, 0),
                   MakeProvider("p.both", "Provider B", kCtl | kHands, 0, /*present=*/false)};
    g.providers[1].isUserDefault = XR_TRUE;
    InputProviderClient c;
    c.Attach(FakeApi(), kInstance, kSystem, kSession);
    CHECK(c.Visible() && c.Providers().size() == 4, "four providers listed");
    CHECK(c.Providers()[0].name == "Provider H" && c.Providers()[3].id == "p.both",
          "rank order + verbatim strings");
    CHECK(!c.Providers()[3].present && c.Providers()[1].userDefault, "presence + default carried");
    CHECK(c.FamilyOffered(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR) &&
              c.FamilyOffered(XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR) &&
              !c.FamilyOffered(XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR),
          "families offered");
    CHECK(c.Current(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR) == 1, "controllers holder = isCurrent");
    CHECK(c.Current(XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR) == 0, "hands holder = isCurrent");
    CHECK(c.Current(XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR) == -1, "no gamepad holder");
    CHECK(c.Requested(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR) == -1, "no request yet");
}

void TestRequest() {
    g = Fake{};
    g.providers = {MakeProvider("p.ctl", "Provider C", kCtl, kCtl),
                   MakeProvider("emulated", "Emulated controllers", kCtl, 0)};
    InputProviderClient c;
    c.Attach(FakeApi(), kInstance, kSystem, kSession);
    // Default: NOT persisted.
    CHECK(c.SetPreference(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR, "emulated", false).empty(),
          "request accepted");
    CHECK(g.lastSet.family == XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR &&
              std::string(g.lastSet.providerId) == "emulated" && g.lastSet.persist == XR_FALSE,
          "family + id sent, persist off by default");
    CHECK(c.Requested(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR) == 1, "request read back at once");
    CHECK(c.Current(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR) == 0,
          "holder unchanged until the runtime moves it (next xrSyncActions)");
    // Remember for this app: persist only when asked.
    CHECK(c.SetPreference(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR, "p.ctl", true).empty() &&
              g.lastSet.persist == XR_TRUE,
          "persist passed through when the user asks");
    // Back to the default = empty id.
    CHECK(c.SetPreference(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR, "", false).empty() &&
              g.lastSet.providerId[0] == '\0' &&
              c.Requested(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR) == -1,
          "empty id clears the request");
    // Refusals.
    g.setResult = XR_ERROR_FEATURE_UNSUPPORTED;
    CHECK(!c.SetPreference(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR, "p.ctl", true).empty(),
          "FEATURE_UNSUPPORTED surfaces as a reason");
    g.setResult = XR_SUCCESS;
    CHECK(!c.SetPreference((XrInputRoleFamilyDXR)99, "p.ctl", false).empty(),
          "unknown family refused before the call");
    const std::string longId(80, 'x');
    CHECK(!c.SetPreference(XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR, longId, false).empty(),
          "over-long id refused");
}

void TestEvent() {
    g = Fake{};
    g.providers = {MakeProvider("p.ctl", "Provider C", kCtl, kCtl)};
    InputProviderClient c;
    c.Attach(FakeApi(), kInstance, kSystem, kSession);
    // Hardware arrives and takes the hands for this app: the event says re-enumerate.
    g.providers.push_back(MakeProvider("p.hands", "Provider H", kHands, kHands));
    XrEventDataInputProvidersChangedDXR e{};
    e.type = XR_TYPE_EVENT_DATA_INPUT_PROVIDERS_CHANGED_DXR;
    e.changedFamilies = kHands;
    const int calls = g.enumCalls;
    CHECK(c.OnEvent(reinterpret_cast<const XrEventDataBaseHeader*>(&e)), "event consumed");
    CHECK(g.enumCalls > calls && c.Providers().size() == 2 &&
              c.Current(XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR) == 1,
          "event refreshes the list");
    XrEventDataBuffer other{};
    other.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    CHECK(!c.OnEvent(reinterpret_cast<const XrEventDataBaseHeader*>(&other)),
          "other events ignored");
    c.Detach();
    CHECK(!c.Visible() && c.Providers().empty(), "detach forgets the list");
}

void TestFamilyKeys() {
    XrInputRoleFamilyDXR f;
    CHECK(ParseFamily("controllers", f) && f == XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR,
          "controllers");
    CHECK(ParseFamily("hand_tracking", f) && f == XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR,
          "hand_tracking");
    CHECK(ParseFamily("gamepad", f) && f == XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR, "gamepad");
    CHECK(!ParseFamily("head", f), "the head is never selectable");
    CHECK(std::string(FamilyKey(XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR)) == "hand_tracking", "key");
}

}  // namespace

int main() {
    TestHidden();
    TestListAndCurrent();
    TestRequest();
    TestEvent();
    TestFamilyKeys();
    if (g_failures) {
        std::fprintf(stderr, "input_provider_client_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("input_provider_client_test: all passed\n");
    return EXIT_SUCCESS;
}
