// SPDX-License-Identifier: Apache-2.0
#include "browser/AndroidMediaProvider.h"

#include <jni.h>

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

void AndroidMediaProvider::WantThumbnails(const std::vector<int64_t>& ids, uint32_t maxEdge) {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_ = ids;
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

bool AndroidMediaProvider::TakeWantedThumbnails(std::vector<int64_t>& ids, uint32_t& maxEdge) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wantedChanged_) return false;
    wantedChanged_ = false;
    ids = wanted_;
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

} // namespace mp::browser

// ─── JNI: MediaStoreProvider.kt ──────────────────────────────────────────

namespace {

using mp::browser::AndroidMediaProvider;

std::string StringAt(JNIEnv* env, jobjectArray array, jsize i) {
    jstring s = (jstring)env->GetObjectArrayElement(array, i);
    std::string out;
    if (s) {
        const char* c = env->GetStringUTFChars(s, nullptr);
        if (c) {
            out = c;
            env->ReleaseStringUTFChars(s, c);
        }
        env->DeleteLocalRef(s);  // a long list would overflow the local reference table
    }
    return out;
}

std::vector<jlong> Longs(JNIEnv* env, jlongArray array) {
    std::vector<jlong> out(env->GetArrayLength(array));
    env->GetLongArrayRegion(array, 0, (jsize)out.size(), out.data());
    return out;
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
    jlongArray modified, jlongArray sizes, jlongArray durations, jlongArray albumIds, jobjectArray albums,
    jobjectArray folders)
{
    const std::vector<jlong> id = Longs(env, ids), date = Longs(env, dates), mod = Longs(env, modified),
                             size = Longs(env, sizes), duration = Longs(env, durations),
                             albumId = Longs(env, albumIds);
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
        item.albumId = albumId[i];
        item.album = StringAt(env, albums, (jsize)i);
        item.folder = StringAt(env, folders, (jsize)i);
    }
    AndroidMediaProvider::Instance().SetItems(std::move(items));
}

// `rgba` is width x height x 4 bytes, or null (0 x 0) when no thumbnail could be made.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeThumbnail(
    JNIEnv* env, jobject /*thiz*/, jlong id, jint width, jint height, jbyteArray rgba)
{
    mp::browser::Thumbnail t;
    t.id = id;
    if (rgba && width > 0 && height > 0) {
        t.width = (uint32_t)width;
        t.height = (uint32_t)height;
        t.rgba.resize((size_t)width * height * 4);
        env->GetByteArrayRegion(rgba, 0, (jsize)t.rgba.size(), (jbyte*)t.rgba.data());
    }
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

// The new wanted list, or null if it has not changed. edgeOut[0] receives the max edge.
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_displayxr_mediaplayer_1vk_1android_MediaStoreProvider_nativeTakeWantedThumbnails(
    JNIEnv* env, jobject /*thiz*/, jintArray edgeOut)
{
    std::vector<int64_t> ids;
    uint32_t maxEdge = 0;
    if (!AndroidMediaProvider::Instance().TakeWantedThumbnails(ids, maxEdge)) return nullptr;
    const jint edge = (jint)maxEdge;
    env->SetIntArrayRegion(edgeOut, 0, 1, &edge);
    jlongArray out = env->NewLongArray((jsize)ids.size());
    env->SetLongArrayRegion(out, 0, (jsize)ids.size(), (const jlong*)ids.data());
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
