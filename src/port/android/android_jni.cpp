// JNI side of NSWHomeActivity.java: see android_jni.h and ../port.h.
#include "android_jni.h"
#include "../port.h"

#include <jni.h>
#include <pthread.h>

#include <cstdlib>
#include <deque>
#include <mutex>
#include <utility>

namespace {

JavaVM* vm = nullptr;
jclass activityClass = nullptr; // global reference: native threads cannot look app classes up by name
pthread_key_t envKey;

jmethodID mFilesDir, mDebugExtras, mQuit, mPickStart, mPickPoll, mGrantedRoots, mListDir, mStatPath, mOpenRead,
    mEmulatorLabel, mLaunch, mBattery, mSafeInsets;

std::mutex grantLock;
std::vector<std::string> grantRoots;
bool grantsRead = false;

std::mutex debugLock;
std::deque<std::pair<std::string, std::string>> debugQueue;
void (*debugWake)() = nullptr;
void (*padKeySink)(int, bool) = nullptr;

void DetachThread(void*) {
    if (vm) vm->DetachCurrentThread();
}

// The JNI environment of the calling thread, attaching it on first use (the
// library scan runs on a thread Java knows nothing about).
JNIEnv* Env() {
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(envKey, env); // detached again when the thread ends
    return env;
}

// Java strings are UTF-16, and the JNI's "UTF" calls use a modified UTF-8
// that differs for characters outside the basic plane (they do occur in game
// names). Going through UTF-16 is exact.
jstring JStr(JNIEnv* env, const std::string& utf8) {
    std::wstring wide = port::Wide(utf8);
    std::vector<jchar> units;
    units.reserve(wide.size());
    for (wchar_t c : wide) {
        uint32_t cp = static_cast<uint32_t>(c);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            units.push_back(static_cast<jchar>(0xD800 + (cp >> 10)));
            units.push_back(static_cast<jchar>(0xDC00 + (cp & 0x3FF)));
        } else {
            units.push_back(static_cast<jchar>(cp));
        }
    }
    return env->NewString(units.data(), static_cast<jsize>(units.size()));
}

std::string Str(JNIEnv* env, jstring s) {
    if (!s) return {};
    jsize n = env->GetStringLength(s);
    const jchar* units = env->GetStringChars(s, nullptr);
    std::wstring wide;
    for (jsize i = 0; units && i < n; i++) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[i + 1] - 0xDC00);
            i++;
        }
        wide.push_back(static_cast<wchar_t>(cp));
    }
    if (units) env->ReleaseStringChars(s, units);
    return port::Utf8(wide);
}

// False (and the exception dropped) when the Java call threw.
bool Cleared(JNIEnv* env) {
    if (!env->ExceptionCheck()) return true;
    env->ExceptionClear();
    return false;
}

std::vector<std::string> StringArray(JNIEnv* env, jobjectArray arr) {
    std::vector<std::string> out;
    if (!arr) return out;
    jsize n = env->GetArrayLength(arr);
    out.reserve(static_cast<size_t>(n));
    for (jsize i = 0; i < n; i++) {
        jstring s = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
        out.push_back(Str(env, s));
        if (s) env->DeleteLocalRef(s);
    }
    return out;
}

// Calls a static method that takes one path and returns String[] (null = failure).
bool CallList(jmethodID method, const std::string& path, std::vector<std::string>& out) {
    JNIEnv* env = Env();
    if (!env || !activityClass) return false;
    jstring jpath = JStr(env, path);
    jobjectArray arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(activityClass, method, jpath));
    env->DeleteLocalRef(jpath);
    if (!Cleared(env) || !arr) return false;
    out = StringArray(env, arr);
    env->DeleteLocalRef(arr);
    return true;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* javaVm, void*) {
    vm = javaVm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    pthread_key_create(&envKey, DetachThread);
    jclass local = env->FindClass("app/nswhome/NSWHomeActivity");
    if (!local) return JNI_ERR;
    activityClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    auto method = [&](const char* name, const char* sig) { return env->GetStaticMethodID(activityClass, name, sig); };
    mFilesDir = method("filesDir", "()Ljava/lang/String;");
    mDebugExtras = method("debugExtras", "()[Ljava/lang/String;");
    mQuit = method("quit", "()V");
    mPickStart = method("pickStart", "(I)V");
    mPickPoll = method("pickPoll", "()Ljava/lang/String;");
    mGrantedRoots = method("grantedRoots", "()[Ljava/lang/String;");
    mListDir = method("listDir", "(Ljava/lang/String;)[Ljava/lang/String;");
    mStatPath = method("statPath", "(Ljava/lang/String;)[Ljava/lang/String;");
    mOpenRead = method("openRead", "(Ljava/lang/String;)I");
    mEmulatorLabel = method("emulatorLabel", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    mLaunch = method("launch", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)"
                               "Ljava/lang/String;");
    mBattery = method("battery", "()I");
    mSafeInsets = method("safeInsets", "()[I");
    if (!Cleared(env)) return JNI_ERR;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL Java_app_nswhome_NSWHomeActivity_nativeDebug(JNIEnv* env, jclass, jstring command,
                                                                                jstring argument) {
    void (*wake)() = nullptr;
    {
        std::lock_guard<std::mutex> guard(debugLock);
        debugQueue.emplace_back(Str(env, command), Str(env, argument));
        wake = debugWake;
    }
    if (wake) wake();
}

extern "C" JNIEXPORT void JNICALL Java_app_nswhome_NSWHomeActivity_nativePadKey(JNIEnv*, jclass, jint keyCode,
                                                                                 jboolean down) {
    void (*sink)(int, bool) = nullptr;
    {
        std::lock_guard<std::mutex> guard(debugLock);
        sink = padKeySink;
    }
    if (sink) sink(static_cast<int>(keyCode), down != JNI_FALSE);
}

namespace port {
namespace android {

void SetPadKeySink(void (*sink)(int keyCode, bool down)) {
    std::lock_guard<std::mutex> guard(debugLock);
    padKeySink = sink;
}

void SafeInsets(int out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    JNIEnv* env = Env();
    if (!env || !activityClass) return;
    jintArray arr = static_cast<jintArray>(env->CallStaticObjectMethod(activityClass, mSafeInsets));
    if (!Cleared(env) || !arr) return;
    if (env->GetArrayLength(arr) >= 4) {
        jint v[4];
        env->GetIntArrayRegion(arr, 0, 4, v);
        for (int i = 0; i < 4; i++) out[i] = v[i] > 0 ? static_cast<int>(v[i]) : 0;
    }
    env->DeleteLocalRef(arr);
}

std::string FilesDir() {
    JNIEnv* env = Env();
    if (!env || !activityClass) return {};
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(activityClass, mFilesDir));
    if (!Cleared(env)) return {};
    std::string out = Str(env, s);
    if (s) env->DeleteLocalRef(s);
    return out;
}

std::vector<std::string> DebugExtras() {
    JNIEnv* env = Env();
    if (!env || !activityClass) return {};
    jobjectArray arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(activityClass, mDebugExtras));
    if (!Cleared(env) || !arr) return {};
    std::vector<std::string> out = StringArray(env, arr);
    env->DeleteLocalRef(arr);
    return out;
}

bool PopDebug(std::string& command, std::string& argument) {
    std::lock_guard<std::mutex> guard(debugLock);
    if (debugQueue.empty()) return false;
    command = std::move(debugQueue.front().first);
    argument = std::move(debugQueue.front().second);
    debugQueue.pop_front();
    return true;
}

void SetDebugWake(void (*wake)()) {
    std::lock_guard<std::mutex> guard(debugLock);
    debugWake = wake;
}

void Quit() {
    JNIEnv* env = Env();
    if (!env || !activityClass) return;
    env->CallStaticVoidMethod(activityClass, mQuit);
    Cleared(env);
}

void PickStart(PickKind kind) {
    JNIEnv* env = Env();
    if (!env || !activityClass) return;
    env->CallStaticVoidMethod(activityClass, mPickStart, static_cast<jint>(kind));
    Cleared(env);
}

bool PickPoll(std::string& result) {
    result.clear();
    JNIEnv* env = Env();
    if (!env || !activityClass) return true;
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(activityClass, mPickPoll));
    if (!Cleared(env)) return true;
    if (!s) return false; // still open
    result = Str(env, s);
    env->DeleteLocalRef(s);
    return true;
}

void RefreshGrants() {
    JNIEnv* env = Env();
    std::vector<std::string> roots;
    if (env && activityClass) {
        jobjectArray arr = static_cast<jobjectArray>(env->CallStaticObjectMethod(activityClass, mGrantedRoots));
        if (Cleared(env) && arr) {
            roots = StringArray(env, arr);
            env->DeleteLocalRef(arr);
        }
    }
    std::lock_guard<std::mutex> guard(grantLock);
    grantRoots = std::move(roots);
    grantsRead = true;
}

bool Granted(const std::string& path) {
    {
        std::lock_guard<std::mutex> guard(grantLock);
        if (grantsRead) {
            for (const std::string& root : grantRoots)
                if (path.compare(0, root.size(), root) == 0 && (path.size() == root.size() || path[root.size()] == '/'))
                    return true;
            return false;
        }
    }
    RefreshGrants();
    return Granted(path);
}

bool List(const std::string& path, std::vector<Entry>& out) {
    std::vector<std::string> flat;
    if (!CallList(mListDir, path, flat)) return false;
    out.clear();
    for (size_t i = 0; i + 3 < flat.size(); i += 4) {
        Entry e;
        e.name = flat[i];
        e.dir = flat[i + 1] == "d";
        e.size = strtoull(flat[i + 2].c_str(), nullptr, 10);
        e.mtimeMs = strtoll(flat[i + 3].c_str(), nullptr, 10);
        out.push_back(std::move(e));
    }
    return true;
}

bool Stat(const std::string& path, Entry& out) {
    std::vector<std::string> flat;
    if (!CallList(mStatPath, path, flat) || flat.size() < 3) return false;
    size_t slash = path.find_last_of('/');
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    out.dir = flat[0] == "d";
    out.size = strtoull(flat[1].c_str(), nullptr, 10);
    out.mtimeMs = strtoll(flat[2].c_str(), nullptr, 10);
    return true;
}

int OpenRead(const std::string& path) {
    JNIEnv* env = Env();
    if (!env || !activityClass) return -1;
    jstring jpath = JStr(env, path);
    jint fd = env->CallStaticIntMethod(activityClass, mOpenRead, jpath);
    env->DeleteLocalRef(jpath);
    return Cleared(env) ? static_cast<int>(fd) : -1;
}

bool Battery(int& percent, bool& charging) {
    JNIEnv* env = Env();
    if (!env || !activityClass) return false;
    jint v = env->CallStaticIntMethod(activityClass, mBattery);
    if (!Cleared(env) || v < 0) return false;
    percent = v / 2;
    charging = (v & 1) != 0;
    return true;
}

bool EmulatorInstalled(const std::string& package, const std::string& activity, std::string& label) {
    JNIEnv* env = Env();
    if (!env || !activityClass) return false;
    jstring jpkg = JStr(env, package), jcls = JStr(env, activity);
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(activityClass, mEmulatorLabel, jpkg, jcls));
    env->DeleteLocalRef(jpkg);
    env->DeleteLocalRef(jcls);
    if (!Cleared(env) || !s) return false;
    label = Str(env, s);
    env->DeleteLocalRef(s);
    return true;
}

std::string Launch(const std::string& package, const std::string& activity, const std::string& action,
                   const std::string& extra, const std::string& gamePath) {
    JNIEnv* env = Env();
    if (!env || !activityClass) return "NO_ACTIVITY";
    jstring a[5] = {JStr(env, package), JStr(env, activity), JStr(env, action), JStr(env, extra), JStr(env, gamePath)};
    jstring s = static_cast<jstring>(env->CallStaticObjectMethod(activityClass, mLaunch, a[0], a[1], a[2], a[3], a[4]));
    for (jstring j : a) env->DeleteLocalRef(j);
    if (!Cleared(env)) return "EXCEPTION";
    std::string out = Str(env, s);
    if (s) env->DeleteLocalRef(s);
    return out;
}

} // namespace android
} // namespace port
