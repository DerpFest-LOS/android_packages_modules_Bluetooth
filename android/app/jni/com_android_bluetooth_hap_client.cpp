/*
 * Copyright 2021 HIMSA II K/S - www.himsa.com.
 * Represented by EHIMA - www.ehima.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "BluetoothHapClientJni"

#include <bluetooth/log.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/scoped_local_ref.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <variant>
#include <vector>

#include "com_android_bluetooth.h"
#include "hardware/bluetooth.h"
#include "hardware/bt_has.h"
#include "types/raw_address.h"

using bluetooth::has::ConnectionState;
using bluetooth::has::ErrorCode;
using bluetooth::has::HasClientCallbacks;
using bluetooth::has::HasClientInterface;
using bluetooth::has::PresetInfo;
using bluetooth::has::PresetInfoReason;

namespace android {
static jmethodID method_onConnectionStateChanged;
static jmethodID method_onDeviceAvailable;
static jmethodID method_onFeaturesUpdate;
static jmethodID method_onActivePresetSelected;
static jmethodID method_onGroupActivePresetSelected;
static jmethodID method_onActivePresetSelectError;
static jmethodID method_onGroupActivePresetSelectError;
static jmethodID method_onPresetInfo;
static jmethodID method_onGroupPresetInfo;
static jmethodID method_onPresetInfoError;
static jmethodID method_onGroupPresetInfoError;
static jmethodID method_onPresetNameSetError;
static jmethodID method_onGroupPresetNameSetError;

static HasClientInterface* sHasClientInterface = nullptr;
static std::shared_timed_mutex interface_mutex;

static jobject mCallbacksObj = nullptr;
static std::shared_timed_mutex callbacks_mutex;
static jfieldID sCallbacksField;

static struct {
  jclass clazz;
  jmethodID constructor;
  jmethodID getCodecType;
  jmethodID getCodecPriority;
  jmethodID getSampleRate;
  jmethodID getBitsPerSample;
  jmethodID getChannelMode;
  jmethodID getCodecSpecific1;
  jmethodID getCodecSpecific2;
  jmethodID getCodecSpecific3;
  jmethodID getCodecSpecific4;
} android_bluetooth_BluetoothHapPresetInfo;

class HasClientCallbacksImpl : public HasClientCallbacks {
public:
  ~HasClientCallbacksImpl() = default;

  void OnConnectionState(ConnectionState state, const RawAddress& bd_addr) override {
    log::info("");

    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                    sCallbackEnv->NewByteArray(sizeof(RawAddress)));
    if (!addr.get()) {
      log::error("Failed to new bd addr jbyteArray for connection state");
      return;
    }

    sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                     reinterpret_cast<const jbyte*>(&bd_addr));
    sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onConnectionStateChanged, (jint)state,
                                 addr.get());
  }

  void OnDeviceAvailable(const RawAddress& bd_addr, uint8_t features) override {
    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                    sCallbackEnv->NewByteArray(sizeof(RawAddress)));
    if (!addr.get()) {
      log::error("Failed to new bd addr jbyteArray for device available");
      return;
    }
    sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                     reinterpret_cast<const jbyte*>(&bd_addr));

    sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onDeviceAvailable, addr.get(),
                                 (jint)features);
  }

  void OnFeaturesUpdate(const RawAddress& bd_addr, uint8_t features) override {
    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                    sCallbackEnv->NewByteArray(sizeof(RawAddress)));
    if (!addr.get()) {
      log::error("Failed to new bd addr jbyteArray for device available");
      return;
    }
    sCallbackEnv->SetByteArrayRegion(addr.get(), 0, sizeof(RawAddress),
                                     reinterpret_cast<const jbyte*>(&bd_addr));

    sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onFeaturesUpdate, addr.get(),
                                 (jint)features);
  }

  void OnActivePresetSelected(std::variant<RawAddress, int> addr_or_group_id,
                              uint8_t preset_index) override {
    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                      sCallbackEnv->NewByteArray(sizeof(RawAddress)));
      if (!addr.get()) {
        log::error("Failed to new bd addr jbyteArray for preset selected");
        return;
      }
      sCallbackEnv->SetByteArrayRegion(
              addr.get(), 0, sizeof(RawAddress),
              reinterpret_cast<jbyte*>(&std::get<RawAddress>(addr_or_group_id)));

      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onActivePresetSelected, addr.get(),
                                   (jint)preset_index);
    } else {
      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onGroupActivePresetSelected,
                                   std::get<int>(addr_or_group_id), (jint)preset_index);
    }
  }

  void OnActivePresetSelectError(std::variant<RawAddress, int> addr_or_group_id,
                                 ErrorCode error_code) override {
    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                      sCallbackEnv->NewByteArray(sizeof(RawAddress)));
      if (!addr.get()) {
        log::error("Failed to new bd addr jbyteArray for preset select error");
        return;
      }
      sCallbackEnv->SetByteArrayRegion(
              addr.get(), 0, sizeof(RawAddress),
              reinterpret_cast<jbyte*>(&std::get<RawAddress>(addr_or_group_id)));

      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onActivePresetSelectError, addr.get(),
                                   (jint)error_code);
    } else {
      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onGroupActivePresetSelectError,
                                   std::get<int>(addr_or_group_id), (jint)error_code);
    }
  }

  void OnPresetInfo(std::variant<RawAddress, int> addr_or_group_id, PresetInfoReason info_reason,
                    std::vector<PresetInfo> detail_records) override {
    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    jsize i = 0;
    jobjectArray presets_array = sCallbackEnv->NewObjectArray(
            (jsize)detail_records.size(), android_bluetooth_BluetoothHapPresetInfo.clazz, nullptr);

    const char null_str[] = "";
    for (auto const& info : detail_records) {
      const char* name = info.preset_name.c_str();
      if (!sCallbackEnv.isValidUtf(name)) {
        log::error("name is not a valid UTF string.");
        name = null_str;
      }

      ScopedLocalRef<jstring> name_str(sCallbackEnv.get(), sCallbackEnv->NewStringUTF(name));
      if (!name_str.get()) {
        log::error("Failed to new preset name String for preset name");
        return;
      }

      jobject infoObj = sCallbackEnv->NewObject(
              android_bluetooth_BluetoothHapPresetInfo.clazz,
              android_bluetooth_BluetoothHapPresetInfo.constructor, (jint)info.preset_index,
              name_str.get(), (jboolean)info.writable, (jboolean)info.available);
      sCallbackEnv->SetObjectArrayElement(presets_array, i++, infoObj);
      sCallbackEnv->DeleteLocalRef(infoObj);
    }

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                      sCallbackEnv->NewByteArray(sizeof(RawAddress)));
      if (!addr.get()) {
        log::error("Failed to new bd addr jbyteArray for preset name");
        return;
      }
      sCallbackEnv->SetByteArrayRegion(
              addr.get(), 0, sizeof(RawAddress),
              reinterpret_cast<jbyte*>(&std::get<RawAddress>(addr_or_group_id)));

      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onPresetInfo, addr.get(),
                                   (jint)info_reason, presets_array);
    } else {
      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onGroupPresetInfo,
                                   std::get<int>(addr_or_group_id), (jint)info_reason,
                                   presets_array);
    }
  }

  void OnPresetInfoError(std::variant<RawAddress, int> addr_or_group_id, uint8_t preset_index,
                         ErrorCode error_code) override {
    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                      sCallbackEnv->NewByteArray(sizeof(RawAddress)));
      if (!addr.get()) {
        log::error("Failed to new bd addr jbyteArray for preset name get error");
        return;
      }
      sCallbackEnv->SetByteArrayRegion(
              addr.get(), 0, sizeof(RawAddress),
              reinterpret_cast<jbyte*>(&std::get<RawAddress>(addr_or_group_id)));

      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onPresetInfoError, addr.get(),
                                   (jint)preset_index, (jint)error_code);
    } else {
      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onGroupPresetInfoError,
                                   std::get<int>(addr_or_group_id), (jint)preset_index,
                                   (jint)error_code);
    }
  }

  void OnSetPresetNameError(std::variant<RawAddress, int> addr_or_group_id, uint8_t preset_index,
                            ErrorCode error_code) override {
    std::shared_lock<std::shared_timed_mutex> lock(callbacks_mutex);
    CallbackEnv sCallbackEnv(__func__);
    if (!sCallbackEnv.valid() || mCallbacksObj == nullptr) {
      return;
    }

    if (std::holds_alternative<RawAddress>(addr_or_group_id)) {
      ScopedLocalRef<jbyteArray> addr(sCallbackEnv.get(),
                                      sCallbackEnv->NewByteArray(sizeof(RawAddress)));
      if (!addr.get()) {
        log::error("Failed to new bd addr jbyteArray for preset name set error");
        return;
      }
      sCallbackEnv->SetByteArrayRegion(
              addr.get(), 0, sizeof(RawAddress),
              reinterpret_cast<jbyte*>(&std::get<RawAddress>(addr_or_group_id)));

      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onPresetNameSetError, addr.get(),
                                   (jint)preset_index, (jint)error_code);
    } else {
      sCallbackEnv->CallVoidMethod(mCallbacksObj, method_onGroupPresetNameSetError,
                                   std::get<int>(addr_or_group_id), (jint)preset_index,
                                   (jint)error_code);
    }
  }
};

static HasClientCallbacksImpl sHasClientCallbacks;

static void initNative(JNIEnv* env, jobject obj) {
  std::unique_lock<std::shared_timed_mutex> interface_lock(interface_mutex);
  std::unique_lock<std::shared_timed_mutex> callbacks_lock(callbacks_mutex);

  const bt_interface_t* btInf = getBluetoothInterface();
  if (btInf == nullptr) {
    log::error("Bluetooth module is not loaded");
    return;
  }

  if (sHasClientInterface != nullptr) {
    log::info("Cleaning up HearingAid Interface before initializing...");
    sHasClientInterface->Cleanup();
    sHasClientInterface = nullptr;
  }

  if (mCallbacksObj != nullptr) {
    log::info("Cleaning up HearingAid callback object");
    env->DeleteGlobalRef(mCallbacksObj);
    mCallbacksObj = nullptr;
  }

  if ((mCallbacksObj = env->NewGlobalRef(env->GetObjectField(obj, sCallbacksField))) == nullptr) {
    log::error("Failed to allocate Global Ref for Hearing Access Callbacks");
    return;
  }

  android_bluetooth_BluetoothHapPresetInfo.clazz =
          (jclass)env->NewGlobalRef(env->FindClass("android/bluetooth/BluetoothHapPresetInfo"));
  if (android_bluetooth_BluetoothHapPresetInfo.clazz == nullptr) {
    log::error("Failed to allocate Global Ref for BluetoothHapPresetInfo class");
    return;
  }

  sHasClientInterface = const_cast<HasClientInterface*>(reinterpret_cast<const HasClientInterface*>(
          btInf->get_profile_interface(BT_PROFILE_HAP_CLIENT_ID)));
  if (sHasClientInterface == nullptr) {
    log::error("Failed to get Bluetooth Hearing Access Service Client Interface");
    return;
  }

  sHasClientInterface->Init(&sHasClientCallbacks);
}

static void cleanupNative(JNIEnv* env, jobject /* object */) {
  std::unique_lock<std::shared_timed_mutex> interface_lock(interface_mutex);
  std::unique_lock<std::shared_timed_mutex> callbacks_lock(callbacks_mutex);

  const bt_interface_t* btInf = getBluetoothInterface();
  if (btInf == nullptr) {
    log::error("Bluetooth module is not loaded");
    return;
  }

  if (sHasClientInterface != nullptr) {
    sHasClientInterface->Cleanup();
    sHasClientInterface = nullptr;
  }

  if (mCallbacksObj != nullptr) {
    env->DeleteGlobalRef(mCallbacksObj);
    mCallbacksObj = nullptr;
  }
}

static jboolean connectHapClientNative(JNIEnv* env, jobject /* object */, jbyteArray address) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  RawAddress* tmpraw = reinterpret_cast<RawAddress*>(addr);
  sHasClientInterface->Connect(*tmpraw);
  env->ReleaseByteArrayElements(address, addr, 0);
  return JNI_TRUE;
}

static jboolean disconnectHapClientNative(JNIEnv* env, jobject /* object */, jbyteArray address) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return JNI_FALSE;
  }

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) {
    jniThrowIOException(env, EINVAL);
    return JNI_FALSE;
  }

  RawAddress* tmpraw = reinterpret_cast<RawAddress*>(addr);
  sHasClientInterface->Disconnect(*tmpraw);
  env->ReleaseByteArrayElements(address, addr, 0);
  return JNI_TRUE;
}

static void selectActivePresetNative(JNIEnv* env, jobject /* object */, jbyteArray address,
                                     jint preset_index) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) {
    jniThrowIOException(env, EINVAL);
    return;
  }

  RawAddress* tmpraw = reinterpret_cast<RawAddress*>(addr);
  sHasClientInterface->SelectActivePreset(*tmpraw, preset_index);
  env->ReleaseByteArrayElements(address, addr, 0);
}

static void groupSelectActivePresetNative(JNIEnv* /* env */, jobject /* object */, jint group_id,
                                          jint preset_index) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  sHasClientInterface->SelectActivePreset(group_id, preset_index);
}

static void nextActivePresetNative(JNIEnv* env, jobject /* object */, jbyteArray address) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) {
    jniThrowIOException(env, EINVAL);
    return;
  }

  RawAddress* tmpraw = reinterpret_cast<RawAddress*>(addr);
  sHasClientInterface->NextActivePreset(*tmpraw);
  env->ReleaseByteArrayElements(address, addr, 0);
}

static void groupNextActivePresetNative(JNIEnv* /* env */, jobject /* object */, jint group_id) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  sHasClientInterface->NextActivePreset(group_id);
}

static void previousActivePresetNative(JNIEnv* env, jobject /* object */, jbyteArray address) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) {
    jniThrowIOException(env, EINVAL);
    return;
  }

  RawAddress* tmpraw = reinterpret_cast<RawAddress*>(addr);
  sHasClientInterface->PreviousActivePreset(*tmpraw);
  env->ReleaseByteArrayElements(address, addr, 0);
}

static void groupPreviousActivePresetNative(JNIEnv* /* env */, jobject /* object */,
                                            jint group_id) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  sHasClientInterface->PreviousActivePreset(group_id);
}

static void getPresetInfoNative(JNIEnv* env, jobject /* object */, jbyteArray address,
                                jint preset_index) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) {
    jniThrowIOException(env, EINVAL);
    return;
  }

  RawAddress* tmpraw = reinterpret_cast<RawAddress*>(addr);
  sHasClientInterface->GetPresetInfo(*tmpraw, preset_index);
  env->ReleaseByteArrayElements(address, addr, 0);
}

static void setPresetNameNative(JNIEnv* env, jobject /* object */, jbyteArray address,
                                jint preset_index, jstring name) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  jbyte* addr = env->GetByteArrayElements(address, nullptr);
  if (!addr) {
    jniThrowIOException(env, EINVAL);
    return;
  }

  std::string name_str;
  if (name != nullptr) {
    const char* value = env->GetStringUTFChars(name, nullptr);
    name_str = std::string(value);
    env->ReleaseStringUTFChars(name, value);
  }

  RawAddress* tmpraw = reinterpret_cast<RawAddress*>(addr);
  sHasClientInterface->SetPresetName(*tmpraw, preset_index, std::move(name_str));
  env->ReleaseByteArrayElements(address, addr, 0);
}

static void groupSetPresetNameNative(JNIEnv* env, jobject /* object */, jint group_id,
                                     jint preset_index, jstring name) {
  std::shared_lock<std::shared_timed_mutex> lock(interface_mutex);
  if (!sHasClientInterface) {
    log::error("Failed to get the Bluetooth HAP Interface");
    return;
  }

  std::string name_str;
  if (name != nullptr) {
    const char* value = env->GetStringUTFChars(name, nullptr);
    name_str = std::string(value);
    env->ReleaseStringUTFChars(name, value);
  }

  sHasClientInterface->SetPresetName(group_id, preset_index, std::move(name_str));
}

int register_com_android_bluetooth_hap_client(JNIEnv* env) {
  const JNINativeMethod methods[] = {
          {"initNative", "()V", reinterpret_cast<void*>(initNative)},
          {"cleanupNative", "()V", reinterpret_cast<void*>(cleanupNative)},
          {"connectHapClientNative", "([B)Z", reinterpret_cast<void*>(connectHapClientNative)},
          {"disconnectHapClientNative", "([B)Z",
           reinterpret_cast<void*>(disconnectHapClientNative)},
          {"selectActivePresetNative", "([BI)V", reinterpret_cast<void*>(selectActivePresetNative)},
          {"groupSelectActivePresetNative", "(II)V",
           reinterpret_cast<void*>(groupSelectActivePresetNative)},
          {"nextActivePresetNative", "([B)V", reinterpret_cast<void*>(nextActivePresetNative)},
          {"groupNextActivePresetNative", "(I)V",
           reinterpret_cast<void*>(groupNextActivePresetNative)},
          {"previousActivePresetNative", "([B)V",
           reinterpret_cast<void*>(previousActivePresetNative)},
          {"groupPreviousActivePresetNative", "(I)V",
           reinterpret_cast<void*>(groupPreviousActivePresetNative)},
          {"getPresetInfoNative", "([BI)V", reinterpret_cast<void*>(getPresetInfoNative)},
          {"setPresetNameNative", "([BILjava/lang/String;)V",
           reinterpret_cast<void*>(setPresetNameNative)},
          {"groupSetPresetNameNative", "(IILjava/lang/String;)V",
           reinterpret_cast<void*>(groupSetPresetNameNative)},
  };
  const int result = REGISTER_NATIVE_METHODS(
          env, "com/android/bluetooth/hap/HapClientNativeInterface", methods);
  if (result != 0) {
    return result;
  }

  jclass jniHapClientNativeInterfaceClass =
          env->FindClass("com/android/bluetooth/hap/HapClientNativeInterface");
  sCallbacksField = env->GetFieldID(jniHapClientNativeInterfaceClass, "mHapClientNativeCallback",
                                    "Lcom/android/bluetooth/hap/HapClientNativeCallback;");
  env->DeleteLocalRef(jniHapClientNativeInterfaceClass);

  const JNIJavaMethod javaMethods[] = {
          {"onConnectionStateChanged", "(I[B)V", &method_onConnectionStateChanged},
          {"onDeviceAvailable", "([BI)V", &method_onDeviceAvailable},
          {"onFeaturesUpdate", "([BI)V", &method_onFeaturesUpdate},
          {"onActivePresetSelected", "([BI)V", &method_onActivePresetSelected},
          {"onActivePresetGroupSelected", "(II)V", &method_onGroupActivePresetSelected},
          {"onActivePresetSelectError", "([BI)V", &method_onActivePresetSelectError},
          {"onActivePresetGroupSelectError", "(II)V", &method_onGroupActivePresetSelectError},
          {"onPresetInfo", "([BI[Landroid/bluetooth/BluetoothHapPresetInfo;)V",
           &method_onPresetInfo},
          {"onGroupPresetInfo", "(II[Landroid/bluetooth/BluetoothHapPresetInfo;)V",
           &method_onGroupPresetInfo},
          {"onPresetNameSetError", "([BII)V", &method_onPresetNameSetError},
          {"onGroupPresetNameSetError", "(III)V", &method_onGroupPresetNameSetError},
          {"onPresetInfoError", "([BII)V", &method_onPresetInfoError},
          {"onGroupPresetInfoError", "(III)V", &method_onGroupPresetInfoError},
  };
  GET_JAVA_METHODS(env, "com/android/bluetooth/hap/HapClientNativeCallback", javaMethods);

  const JNIJavaMethod javaHapPresetMethods[] = {
          {"<init>", "(ILjava/lang/String;ZZ)V",
           &android_bluetooth_BluetoothHapPresetInfo.constructor},
  };
  GET_JAVA_METHODS(env, "android/bluetooth/BluetoothHapPresetInfo", javaHapPresetMethods);

  return 0;
}
}  // namespace android
