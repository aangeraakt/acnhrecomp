// nn::friends at the API level: a console with no friends, no blocked users and no network account. First needed by
// Animal Crossing: New Horizons, which reads the blocked-user list when the island starts (friend:u was a stub that
// handed out no service object, and GetBlockedUserList read through a null pointer). Online play stays unavailable:
// the calls that need the network fail like a console that never linked a network account.
#include <cstring>

#include <nx/runtime.h>

#include "kernel.h"

namespace nx {

namespace {

constexpr uint32_t kOk = 0;
constexpr uint32_t kNotLinked = 121 | (4 << 9);  // friends: network service account not available

void ok(CPU& c) { c.x[0] = kOk; }

// (int* count, T* out, const Uid&, int offset, int count[, const FriendFilter&]): an empty list.
void empty_list(CPU& c) {
    if (c.x[0]) st<int32_t>(c.x[0], 0);
    c.x[0] = kOk;
}

// (PlayHistoryRegistrationKey* out, const Uid&, bool local): a zero key (0x40 bytes); no one can read it.
void GetPlayHistoryRegistrationKey(CPU& c) {
    if (c.x[0]) std::memset(reinterpret_cast<void*>(c.x[0]), 0, 0x40);
    c.x[0] = kOk;
}

void not_linked(CPU& c) { c.x[0] = kNotLinked; }

// AsyncContext::GetSystemEvent(os::SystemEvent*): an event that is already signaled, so a wait on a request that
// failed at once returns.
void AsyncGetSystemEvent(CPU& c) {
    auto e = std::make_shared<EventState>();
    e->signaled = true;
    auto r = std::make_shared<KEvent>(), w = std::make_shared<KEvent>();
    r->state = w->state = e;
    uint32_t hr = handle_add(r), hw = handle_add(w);
    static const uint64_t attach =
        find_symbol("_ZN2nn2os17AttachSystemEventEPNS0_15SystemEventTypeEjbjbNS0_14EventClearModeE");
    uint64_t saved[8];
    for (int k = 0; k < 8; k++) saved[k] = c.x[k];
    c.x[0] = saved[1], c.x[1] = hr, c.x[2] = 1, c.x[3] = hw, c.x[4] = 1, c.x[5] = 1;
    if (attach) call(c, attach);
    for (int k = 0; k < 8; k++) c.x[k] = saved[k];
    c.x[0] = kOk;
}

}  // namespace

NX_HLE("_ZN2nn7friends10InitializeEv", ok);
NX_HLE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", empty_list);
NX_HLE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", empty_list);
NX_HLE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",
       GetPlayHistoryRegistrationKey);
NX_HLE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",
       ok);
NX_HLE("_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", ok);
NX_HLE("_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", ok);
NX_HLE("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", ok);
NX_HLE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", not_linked);
NX_HLE("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", AsyncGetSystemEvent);
NX_HLE("_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",
       not_linked);
NX_HLE("_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_",
       not_linked);

}  // namespace nx
