// nn::bcat (data delivered by the console's servers: events, news) at the API level: the delivery cache storage mounts but
// is empty, like on a console that has never been online. Every directory and file is "not found" and a sync request
// is refused. First needed by Animal Crossing: New Horizons (the stubbed bcat:u answered Mount with success and the
// SDK's shim then aborted, finding nothing mounted).
#include <nx/runtime.h>

namespace nx {

namespace {

// nn::bcat results: module 122. ResultNotFound (description 30), ResultInternetRequestNotAccepted (description 2).
constexpr uint32_t kNotFound = 122 | (30u << 9);
constexpr uint32_t kNoInternet = 122 | (2u << 9);

void ok(CPU& c) { c.x[0] = 0; }
void not_found(CPU& c) { c.x[0] = kNotFound; }
void no_internet(CPU& c) { c.x[0] = kNoInternet; }

// Result DeliveryCacheDirectory::Read(int* count, DeliveryCacheDirectoryEntry*, int max): nothing.
void DirectoryRead(CPU& c) {
    if (c.x[1]) st<int32_t>(c.x[1], 0);
    c.x[0] = kNotFound;
}

// Result DeliveryCacheFile::Read(size_t* read, int64_t offset, void*, size_t): nothing.
void FileRead(CPU& c) {
    if (c.x[1]) st<uint64_t>(c.x[1], 0);
    c.x[0] = kNotFound;
}

// Digest DeliveryCacheFile::GetDigest() const: 16 bytes returned through x8.
void GetDigest(CPU& c) {
    if (c.x[8]) st<uint64_t>(c.x[8], 0), st<uint64_t>(c.x[8] + 8, 0);
}

}  // namespace

NX_HLE("_ZN2nn4bcat10InitializeEv", ok);
NX_HLE("_ZN2nn4bcat25MountDeliveryCacheStorageEv", ok);
NX_HLE("_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", ok);
NX_HLE("_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", no_internet);
NX_HLE("_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", not_found);
NX_HLE("_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", DirectoryRead);
NX_HLE("_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", not_found);
NX_HLE("_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", FileRead);
NX_HLE("_ZN2nn4bcat17DeliveryCacheFile9GetDigestEv", GetDigest);

}  // namespace nx
