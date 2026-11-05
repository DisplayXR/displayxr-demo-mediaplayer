// SPDX-License-Identifier: Apache-2.0
#include "browser/AndroidMediaProvider.h"

#include "LifLoader.h"
#include "lvf_probe_android.h"

#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <utility>

namespace mp::browser {

AndroidMediaProvider& AndroidMediaProvider::Instance() {
    static AndroidMediaProvider instance;
    return instance;
}

MediaAccess AndroidMediaProvider::Access() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return access_;
}

void AndroidMediaProvider::RequestAccess() {
    std::lock_guard<std::mutex> lock(mutex_);
    accessRequested_ = true;
}

void AndroidMediaProvider::Refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshRequested_ = true;
}

bool AndroidMediaProvider::TakeItems(std::vector<MediaItem>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!itemsChanged_) return false;
    itemsChanged_ = false;
    out = std::move(items_);
    items_.clear();
    return true;
}

void AndroidMediaProvider::WantThumbnails(const std::vector<ThumbnailRequest>& wanted, uint32_t maxEdge) {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_ = wanted;
    wantedEdge_ = maxEdge;
    wantedChanged_ = true;
}

bool AndroidMediaProvider::TakeThumbnail(Thumbnail& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thumbnails_.empty()) return false;
    out = std::move(thumbnails_.front());
    thumbnails_.pop_front();
    return true;
}

void AndroidMediaProvider::Open(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    openRequested_ = true;
    openId_ = id;
}

void AndroidMediaProvider::WantStills(const std::vector<int64_t>& ids, uint32_t maxEdge) {
    std::lock_guard<std::mutex> lock(mutex_);
    stillIds_ = ids;
    stillEdge_ = maxEdge;
    stillsChanged_ = true;
}

bool AndroidMediaProvider::TakeStill(StillSource& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stills_.empty()) return false;
    out = std::move(stills_.front());
    stills_.pop_front();
    return true;
}

void AndroidMediaProvider::SetAccess(MediaAccess access) {
    std::lock_guard<std::mutex> lock(mutex_);
    access_ = access;
}

void AndroidMediaProvider::SetItems(std::vector<MediaItem> items) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_ = std::move(items);
    itemsChanged_ = true;
}

void AndroidMediaProvider::AddThumbnail(Thumbnail thumbnail) {
    std::lock_guard<std::mutex> lock(mutex_);
    thumbnails_.push_back(std::move(thumbnail));
}

bool AndroidMediaProvider::TakeAccessRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(accessRequested_, false);
}

bool AndroidMediaProvider::TakeRefreshRequest() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(refreshRequested_, false);
}

bool AndroidMediaProvider::TakeWantedThumbnails(std::vector<ThumbnailRequest>& wanted, uint32_t& maxEdge) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wantedChanged_) return false;
    wantedChanged_ = false;
    wanted = wanted_;
    maxEdge = wantedEdge_;
    return true;
}

bool AndroidMediaProvider::TakeOpenRequest(int64_t& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!openRequested_) return false;
    openRequested_ = false;
    id = openId_;
    return true;
}

void AndroidMediaProvider::AddStill(StillSource still) {
    std::lock_guard<std::mutex> lock(mutex_);
    stills_.push_back(std::move(still));
}

bool AndroidMediaProvider::TakeWantedStills(std::vector<int64_t>& ids, uint32_t& maxEdge) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stillsChanged_) return false;
    stillsChanged_ = false;
    ids = stillIds_;
    maxEdge = stillEdge_;
    return true;
}

} // namespace mp::browser

// ─── JNI: MediaStoreProvider.kt ──────────────────────────────────────────

namespace {

using mp::browser::AndroidMediaProvider;

std::string String(JNIEnv* env, jstring s) {
    std::string out;
    const char* c = s ? env->GetStringUTFChars(s, nullptr) : nullptr;
    if (c) {
        out = c;
        env->ReleaseStringUTFChars(s, c);
    }
    return out;
}

std::string StringAt(JNIEnv* env, jobjectArray array, jsize i) {
    jstring s = (jstring)env->GetObjectArrayElement(array, i);
    std::string out = String(env, s);
    if (s) env->DeleteLocalRef(s);  // a long list would overflow the local reference table
    return out;
}

std::vector<jlong> Longs(JNIEnv* env, jlongArray array) {
    std::vector<jlong> out(env->GetArrayLength(array));
    env->GetLongArrayRegion(array, 0, (jsize)out.size(), out.data());
    return out;
}

// Is the file itself 3D: a two-track (LVF) video, or a LIF? Closes `fd`.
bool File3D(int fd, bool video) {
    if (fd < 0) return false;
    struct stat st{};
    bool is3D = false;
    if (fstat(fd, &st) == 0) {
        if (video) {
            is3D = mp::LvfProbeAndroid::RunFd(fd, 0, st.st_size).ok;
        } else {
            auto readAt = [fd](uint64_t offset, void* dst, size_t size) {
                return pread(fd, dst, size, (off_t)offset) == (ssize_t)size;
            };
            is3D = mp::LifLoader::IsLif(readAt, (uint64_t)st.st_size);
        }
    }
    close(fd);
    return is3D;
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeSetAccess(
    JNIEnv* /*env*/, jobject /*thiz*/, jint access)
{
    AndroidMediaProvider::Instance().SetAccess((mp::browser::MediaAccess)access);
}

// One call for the whole list, as parallel arrays of equal length.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeSetItems(
    JNIEnv* env, jobject /*thiz*/, jlongArray ids, jobjectArray names, jintArray types, jlongArray dates,
    jlongArray modified, jlongArray sizes, jlongArray durations, jobjectArray folders)
{
    const std::vector<jlong> id = Longs(env, ids), date = Longs(env, dates), mod = Longs(env, modified),
                             size = Longs(env, sizes), duration = Longs(env, durations);
    std::vector<jint> type(id.size());
    env->GetIntArrayRegion(types, 0, (jsize)type.size(), type.data());

    std::vector<mp::browser::MediaItem> items(id.size());
    for (size_t i = 0; i < items.size(); ++i) {
        mp::browser::MediaItem& item = items[i];
        item.id = id[i];
        item.name = StringAt(env, names, (jsize)i);
        item.type = (mp::browser::MediaType)type[i];
        item.dateMs = date[i];
        item.modified = mod[i];
        item.bytes = size[i];
        item.durationMs = duration[i];
        item.folder = StringAt(env, folders, (jsize)i);
    }
    AndroidMediaProvider::Instance().SetItems(std::move(items));
}

// `rgba` is width x height x 4 bytes, or null (0 x 0) when no thumbnail could be made.
// `fd` is the file, opened for this call and closed here (-1 if it could not be opened):
// it is read on this Java thread for Thumbnail::file3D.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeThumbnail(
    JNIEnv* env, jobject /*thiz*/, jlong id, jint width, jint height, jbyteArray rgba, jint fd, jboolean video)
{
    mp::browser::Thumbnail t;
    t.id = id;
    if (rgba && width > 0 && height > 0) {
        t.width = (uint32_t)width;
        t.height = (uint32_t)height;
        t.rgba.resize((size_t)width * height * 4);
        env->GetByteArrayRegion(rgba, 0, (jsize)t.rgba.size(), (jbyte*)t.rgba.data());
    }
    t.file3D = File3D(fd, video != JNI_FALSE);
    AndroidMediaProvider::Instance().AddThumbnail(std::move(t));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeTakeAccessRequest(
    JNIEnv* /*env*/, jobject /*thiz*/)
{
    return AndroidMediaProvider::Instance().TakeAccessRequest() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeTakeRefreshRequest(
    JNIEnv* /*env*/, jobject /*thiz*/)
{
    return AndroidMediaProvider::Instance().TakeRefreshRequest() ? JNI_TRUE : JNI_FALSE;
}

// The new wanted list, or null if it has not changed: pairs of (id, 1 to check the file
// or 0). edgeOut[0] receives the max edge.
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeTakeWantedThumbnails(
    JNIEnv* env, jobject /*thiz*/, jintArray edgeOut)
{
    std::vector<mp::browser::ThumbnailRequest> wanted;
    uint32_t maxEdge = 0;
    if (!AndroidMediaProvider::Instance().TakeWantedThumbnails(wanted, maxEdge)) return nullptr;
    const jint edge = (jint)maxEdge;
    env->SetIntArrayRegion(edgeOut, 0, 1, &edge);
    std::vector<jlong> pairs;
    pairs.reserve(wanted.size() * 2);
    for (const mp::browser::ThumbnailRequest& w : wanted) {
        pairs.push_back((jlong)w.id);
        pairs.push_back(w.checkFile ? 1 : 0);
    }
    jlongArray out = env->NewLongArray((jsize)pairs.size());
    env->SetLongArrayRegion(out, 0, (jsize)pairs.size(), pairs.data());
    return out;
}

// The id to open, or -1 for none.
extern "C" JNIEXPORT jlong JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeTakeOpenRequest(
    JNIEnv* /*env*/, jobject /*thiz*/)
{
    int64_t id = 0;
    return AndroidMediaProvider::Instance().TakeOpenRequest(id) ? (jlong)id : -1;
}

// The new list of stills wanted, or null if it has not changed. edgeOut receives the
// largest video frame's edge.
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeTakeWantedStills(
    JNIEnv* env, jobject /*thiz*/, jintArray edgeOut)
{
    std::vector<int64_t> ids;
    uint32_t maxEdge = 0;
    if (!AndroidMediaProvider::Instance().TakeWantedStills(ids, maxEdge)) return nullptr;
    const jint edge = (jint)maxEdge;
    env->SetIntArrayRegion(edgeOut, 0, 1, &edge);
    std::vector<jlong> out(ids.begin(), ids.end());
    jlongArray array = env->NewLongArray((jsize)out.size());
    env->SetLongArrayRegion(array, 0, (jsize)out.size(), out.data());
    return array;
}

// A still's source: an image's open file (`fd`, which native closes), or a video's first
// frame (`rgba`, width x height x 4 bytes). -1 and null (0 x 0) when there is none.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeStill(
    JNIEnv* env, jobject /*thiz*/, jlong id, jint fd, jint width, jint height, jbyteArray rgba,
    jstring name)
{
    mp::browser::StillSource s;
    s.id = id;
    s.fd = fd;
    if (rgba && width > 0 && height > 0) {
        s.width = (uint32_t)width;
        s.height = (uint32_t)height;
        s.rgba.resize((size_t)width * height * 4);
        env->GetByteArrayRegion(rgba, 0, (jsize)s.rgba.size(), (jbyte*)s.rgba.data());
    }
    s.name = String(env, name);
    AndroidMediaProvider::Instance().AddStill(std::move(s));
}
