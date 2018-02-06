//
// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// WindowSurfaceVkAndroid.h:
//    Defines the class interface for WindowSurfaceVkAndroid, implementing WindowSurfaceVk.
//

#ifndef LIBANGLE_RENDERER_VULKAN_ANDROID_WINDOWSURFACEVKANDROID_H_
#define LIBANGLE_RENDERER_VULKAN_ANDROID_WINDOWSURFACEVKANDROID_H_

#include "libANGLE/renderer/vulkan/SurfaceVk.h"

namespace rx
{

class WindowSurfaceVkAndroid : public WindowSurfaceVk
{
  public:
    WindowSurfaceVkAndroid(const egl::SurfaceState &surfaceState,
                           EGLNativeWindowType window,
                           EGLint width,
                           EGLint height);

  private:
    vk::ErrorOrResult<gl::Extents> createSurfaceVk(RendererVk *renderer) override;
};

}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_ANDROID_WINDOWSURFACEVKANDROID_H_