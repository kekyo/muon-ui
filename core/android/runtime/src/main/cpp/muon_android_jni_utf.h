/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include "muon_android_utf.h"
#include <jni.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

/**
 * Reads a Java string into standard UTF-8 without truncating embedded NUL.
 * @param env Owner-thread JNI environment.
 * @param value Java string, or null for an empty value.
 * @returns UTF-8 bytes; malformed input raises a Java exception and returns empty.
 */
inline std::string MuonAndroidReadJavaString(JNIEnv* env, jstring value) {
  if (!value) return {};
  const auto release = [env, value](const jchar* text) { env->ReleaseStringChars(value, text); };
  const auto text = std::unique_ptr<const jchar, decltype(release)>(env->GetStringChars(value, nullptr), release);
  if (!text) return {};
  try {
    return MuonAndroidEncodeUtf8(std::u16string(text.get(),
        text.get() + static_cast<size_t>(env->GetStringLength(value))));
  } catch (const std::exception& error) {
    const auto type = env->FindClass("java/lang/IllegalArgumentException");
    if (type) { env->ThrowNew(type, error.what()); env->DeleteLocalRef(type); }
    return {};
  }
}

/**
 * Creates a Java string from standard UTF-8 using explicit UTF-16 length.
 * @param env Owner-thread JNI environment.
 * @param value Length-delimited standard UTF-8.
 * @returns New local reference, or null with a pending Java exception on failure.
 */
inline jstring MuonAndroidNewJavaString(JNIEnv* env, std::string_view value) {
  try {
    const auto text = MuonAndroidDecodeUtf8(value);
    if (text.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()))
      throw std::length_error("Java string exceeds its length limit");
    const auto units = std::vector<jchar>(text.begin(), text.end());
    return env->NewString(units.data(), static_cast<jsize>(units.size()));
  } catch (const std::exception& error) {
    const auto type = env->FindClass("java/lang/IllegalArgumentException");
    if (type) { env->ThrowNew(type, error.what()); env->DeleteLocalRef(type); }
    return nullptr;
  }
}
