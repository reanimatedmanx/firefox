/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_GpuProcessD3D11TextureMap_H
#define MOZILLA_GFX_GpuProcessD3D11TextureMap_H

#include <d3d11.h>
#include <unordered_map>

#include "mozilla/DataMutex.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticPtr.h"

namespace mozilla {
namespace layers {

class IMFSampleUsageInfo;

/**
 * A class to manage ID3D11Texture2Ds that is shared without using shared handle
 * in GPU process. On some GPUs, ID3D11Texture2Ds of hardware decoded video
 * frames with zero video frame copy could not use shared handle.
 */
class GpuProcessD3D11TextureMap {
 public:
  static void Init();
  static void Shutdown();
  static GpuProcessD3D11TextureMap* Get() { return sInstance; }
  static GpuProcessTextureId GetNextTextureId();

  GpuProcessD3D11TextureMap();
  ~GpuProcessD3D11TextureMap();

  void Register(GpuProcessTextureId aTextureId, ID3D11Texture2D* aTexture,
                uint32_t aArrayIndex, const gfx::IntSize& aSize,
                RefPtr<IMFSampleUsageInfo> aUsageInfo);
  void Unregister(GpuProcessTextureId aTextureId);

  RefPtr<ID3D11Texture2D> GetTexture(GpuProcessTextureId aTextureId);
  Maybe<HANDLE> GetSharedHandleOfCopiedTexture(GpuProcessTextureId aTextureId);

 private:
  struct TextureHolder {
    TextureHolder(ID3D11Texture2D* aTexture, uint32_t aArrayIndex,
                  const gfx::IntSize& aSize,
                  RefPtr<IMFSampleUsageInfo> aUsageInfo);
    TextureHolder() = default;

    RefPtr<ID3D11Texture2D> mTexture;
    uint32_t mArrayIndex = 0;
    gfx::IntSize mSize;
    RefPtr<IMFSampleUsageInfo> mIMFSampleUsageInfo;
    RefPtr<ID3D11Texture2D> mCopiedTexture;
    Maybe<HANDLE> mCopiedTextureSharedHandle;
  };

  DataMutex<std::unordered_map<GpuProcessTextureId, TextureHolder,
                               GpuProcessTextureId::HashFn>>
      mD3D11TexturesById;

  static StaticAutoPtr<GpuProcessD3D11TextureMap> sInstance;
};

}  // namespace layers
}  // namespace mozilla

#endif /* MOZILLA_GFX_GpuProcessD3D11TextureMap_H */
