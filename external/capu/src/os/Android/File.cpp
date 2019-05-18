/*
 * Copyright (C) 2012 BMW Car IT GmbH
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

#include "ramses-capu/os/File.h"

#include <errno.h>
#include <android/asset_manager.h>
#include <android/log.h>


static int android_read(void* cookie, char* buf, int size) {
  return AAsset_read(reinterpret_cast<AAsset*>(cookie), buf, size);
}

static int android_write(void* , const char* , int ) {
  return EACCES; // can't provide write access to the apk
}

static fpos_t android_seek(void* cookie, fpos_t offset, int whence) {
  return AAsset_seek(reinterpret_cast<AAsset*>(cookie), offset, whence);
}

static int android_close(void* cookie) {
  AAsset_close(reinterpret_cast<AAsset*>(cookie));
  return 0;
}

// must be established by someone else...
AAssetManager* android_asset_manager;
void android_fopen_set_asset_manager(AAssetManager* manager) {
  __android_log_print(ANDROID_LOG_ERROR, "RamsesNativeInterface", "set asset manager inside native!");
  android_asset_manager = manager;
}

FILE* android_fopen(const char* fname, const char* mode) {
  __android_log_print(ANDROID_LOG_ERROR, "RamsesNativeInterface", "try to open a file asset: %s", fname);
  if(mode[0] == 'w') return NULL;

  AAsset* asset = AAssetManager_open(android_asset_manager, fname, 0);
  if(!asset) return NULL;

  return funopen(asset, android_read, android_write, android_seek, android_close);
}
