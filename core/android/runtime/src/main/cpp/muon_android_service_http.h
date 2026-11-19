/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#pragma once
#include "services/muon_service_io.h"
#include <jni.h>

/** Resolves the Java transport on its application class loader for use on the Android service executor. */
MuonServiceHttpTransport CreateAndroidMuonServiceHttp(JNIEnv* env);
