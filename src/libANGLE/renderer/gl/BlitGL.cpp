//
// Copyright 2015 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// BlitGL.cpp: Implements the BlitGL class, a helper for blitting textures

#include "libANGLE/renderer/gl/BlitGL.h"

#include "common/FixedVector.h"
#include "common/utilities.h"
#include "common/vector_utils.h"
#include "image_util/copyimage.h"
#include "libANGLE/Context.h"
#include "libANGLE/Framebuffer.h"
#include "libANGLE/formatutils.h"
#include "libANGLE/renderer/Format.h"
#include "libANGLE/renderer/gl/FramebufferGL.h"
#include "libANGLE/renderer/gl/FunctionsGL.h"
#include "libANGLE/renderer/gl/RenderbufferGL.h"
#include "libANGLE/renderer/gl/StateManagerGL.h"
#include "libANGLE/renderer/gl/TextureGL.h"
#include "libANGLE/renderer/gl/WorkaroundsGL.h"
#include "libANGLE/renderer/gl/formatutilsgl.h"
#include "libANGLE/renderer/gl/renderergl_utils.h"
#include "libANGLE/renderer/renderer_utils.h"

using angle::Vector2;

namespace rx
{

namespace
{

gl::Error CheckCompileStatus(const rx::FunctionsGL *functions, GLuint shader)
{
    GLint compileStatus = GL_FALSE;
    functions->getShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);

    ASSERT(compileStatus == GL_TRUE);
    if (compileStatus == GL_FALSE)
    {
        return gl::OutOfMemory() << "Failed to compile internal blit shader.";
    }

    return gl::NoError();
}

gl::Error CheckLinkStatus(const rx::FunctionsGL *functions, GLuint program)
{
    GLint linkStatus = GL_FALSE;
    functions->getProgramiv(program, GL_LINK_STATUS, &linkStatus);
    ASSERT(linkStatus == GL_TRUE);
    if (linkStatus == GL_FALSE)
    {
        return gl::OutOfMemory() << "Failed to link internal blit program.";
    }

    return gl::NoError();
}

class ScopedGLState : angle::NonCopyable
{
  public:
    enum
    {
        KEEP_SCISSOR = 1,
    };

    ScopedGLState(StateManagerGL *stateManager,
                  const FunctionsGL *functions,
                  gl::Rectangle viewport,
                  int keepState = 0)
        : mStateManager(stateManager), mFunctions(functions)
    {
        if (!(keepState & KEEP_SCISSOR))
        {
            mStateManager->setScissorTestEnabled(false);
        }
        mStateManager->setViewport(viewport);
        mStateManager->setDepthRange(0.0f, 1.0f);
        mStateManager->setBlendEnabled(false);
        mStateManager->setColorMask(true, true, true, true);
        mStateManager->setSampleAlphaToCoverageEnabled(false);
        mStateManager->setSampleCoverageEnabled(false);
        mStateManager->setDepthTestEnabled(false);
        mStateManager->setStencilTestEnabled(false);
        mStateManager->setCullFaceEnabled(false);
        mStateManager->setPolygonOffsetFillEnabled(false);
        mStateManager->setRasterizerDiscardEnabled(false);

        mStateManager->pauseTransformFeedback();
        ANGLE_SWALLOW_ERR(mStateManager->pauseAllQueries());
    }

    ~ScopedGLState()
    {
        // XFB resuming will be done automatically
        ANGLE_SWALLOW_ERR(mStateManager->resumeAllQueries());
    }

    void willUseTextureUnit(int unit)
    {
        if (mFunctions->bindSampler)
        {
            mStateManager->bindSampler(unit, 0);
        }
    }

  private:
    StateManagerGL *mStateManager;
    const FunctionsGL *mFunctions;
};

gl::Error SetClearState(StateManagerGL *stateManager,
                        bool colorClear,
                        bool depthClear,
                        bool stencilClear,
                        GLbitfield *outClearMask)
{
    *outClearMask = 0;
    if (colorClear)
    {
        stateManager->setClearColor(gl::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        stateManager->setColorMask(true, true, true, true);
        *outClearMask |= GL_COLOR_BUFFER_BIT;
    }
    if (depthClear)
    {
        stateManager->setDepthMask(true);
        stateManager->setClearDepth(1.0f);
        *outClearMask |= GL_DEPTH_BUFFER_BIT;
    }
    if (stencilClear)
    {
        stateManager->setClearStencil(0);
        *outClearMask |= GL_STENCIL_BUFFER_BIT;
    }

    stateManager->setScissorTestEnabled(false);

    return gl::NoError();
}

using ClearBindTargetVector = angle::FixedVector<GLenum, 3>;

gl::Error PrepareForClear(StateManagerGL *stateManager,
                          GLenum sizedInternalFormat,
                          ClearBindTargetVector *outBindtargets,
                          GLbitfield *outClearMask)
{
    const gl::InternalFormat &internalFormatInfo =
        gl::GetSizedInternalFormatInfo(sizedInternalFormat);
    bool bindDepth   = internalFormatInfo.depthBits > 0;
    bool bindStencil = internalFormatInfo.stencilBits > 0;
    bool bindColor   = !bindDepth && !bindStencil;

    outBindtargets->clear();
    if (bindColor)
    {
        outBindtargets->push_back(GL_COLOR_ATTACHMENT0);
    }
    if (bindDepth)
    {
        outBindtargets->push_back(GL_DEPTH_ATTACHMENT);
    }
    if (bindStencil)
    {
        outBindtargets->push_back(GL_STENCIL_ATTACHMENT);
    }

    ANGLE_TRY(SetClearState(stateManager, bindColor, bindDepth, bindStencil, outClearMask));

    return gl::NoError();
}

void UnbindAttachments(const FunctionsGL *functions,
                       GLenum framebufferTarget,
                       const ClearBindTargetVector &bindTargets)
{
    for (GLenum bindTarget : bindTargets)
    {
        functions->framebufferRenderbuffer(framebufferTarget, bindTarget, GL_RENDERBUFFER, 0);
    }
}

}  // anonymous namespace

BlitGL::BlitGL(const FunctionsGL *functions,
               const WorkaroundsGL &workarounds,
               StateManagerGL *stateManager)
    : mFunctions(functions),
      mWorkarounds(workarounds),
      mStateManager(stateManager),
      mScratchFBO(0),
      mVAO(0),
      mVertexBuffer(0)
{
    for (size_t i = 0; i < ArraySize(mScratchTextures); i++)
    {
        mScratchTextures[i] = 0;
    }

    ASSERT(mFunctions);
    ASSERT(mStateManager);
}

BlitGL::~BlitGL()
{
    for (const auto &blitProgram : mBlitPrograms)
    {
        mStateManager->deleteProgram(blitProgram.second.program);
    }
    mBlitPrograms.clear();

    for (size_t i = 0; i < ArraySize(mScratchTextures); i++)
    {
        if (mScratchTextures[i] != 0)
        {
            mStateManager->deleteTexture(mScratchTextures[i]);
            mScratchTextures[i] = 0;
        }
    }

    if (mScratchFBO != 0)
    {
        mStateManager->deleteFramebuffer(mScratchFBO);
        mScratchFBO = 0;
    }

    if (mVAO != 0)
    {
        mStateManager->deleteVertexArray(mVAO);
        mVAO = 0;
    }
}

gl::Error BlitGL::copyImageToLUMAWorkaroundTexture(const gl::Context *context,
                                                   GLuint texture,
                                                   gl::TextureType textureType,
                                                   gl::TextureTarget target,
                                                   GLenum lumaFormat,
                                                   size_t level,
                                                   const gl::Rectangle &sourceArea,
                                                   GLenum internalFormat,
                                                   gl::Framebuffer *source)
{
    mStateManager->bindTexture(textureType, texture);

    // Allocate the texture memory
    GLenum format = gl::GetUnsizedFormat(internalFormat);

    GLenum readType = GL_NONE;
    ANGLE_TRY(source->getImplementationColorReadType(context, &readType));

    gl::PixelUnpackState unpack;
    mStateManager->setPixelUnpackState(unpack);
    mStateManager->setPixelUnpackBuffer(
        context->getGLState().getTargetBuffer(gl::BufferBinding::PixelUnpack));
    mFunctions->texImage2D(ToGLenum(target), static_cast<GLint>(level), internalFormat,
                           sourceArea.width, sourceArea.height, 0, format, readType, nullptr);

    return copySubImageToLUMAWorkaroundTexture(context, texture, textureType, target, lumaFormat,
                                               level, gl::Offset(0, 0, 0), sourceArea, source);
}

gl::Error BlitGL::copySubImageToLUMAWorkaroundTexture(const gl::Context *context,
                                                      GLuint texture,
                                                      gl::TextureType textureType,
                                                      gl::TextureTarget target,
                                                      GLenum lumaFormat,
                                                      size_t level,
                                                      const gl::Offset &destOffset,
                                                      const gl::Rectangle &sourceArea,
                                                      gl::Framebuffer *source)
{
    ANGLE_TRY(initializeResources());

    BlitProgram *blitProgram = nullptr;
    ANGLE_TRY(getBlitProgram(BlitProgramType::FLOAT_TO_FLOAT, &blitProgram));

    // Blit the framebuffer to the first scratch texture
    const FramebufferGL *sourceFramebufferGL = GetImplAs<FramebufferGL>(source);
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, sourceFramebufferGL->getFramebufferID());

    GLenum readFormat = GL_NONE;
    ANGLE_TRY(source->getImplementationColorReadFormat(context, &readFormat));

    GLenum readType = GL_NONE;
    ANGLE_TRY(source->getImplementationColorReadType(context, &readType));

    nativegl::CopyTexImageImageFormat copyTexImageFormat =
        nativegl::GetCopyTexImageImageFormat(mFunctions, mWorkarounds, readFormat, readType);

    mStateManager->bindTexture(gl::TextureType::_2D, mScratchTextures[0]);
    mFunctions->copyTexImage2D(GL_TEXTURE_2D, 0, copyTexImageFormat.internalFormat, sourceArea.x,
                               sourceArea.y, sourceArea.width, sourceArea.height, 0);

    // Set the swizzle of the scratch texture so that the channels sample into the correct emulated
    // LUMA channels.
    GLint swizzle[4] = {
        (lumaFormat == GL_ALPHA) ? GL_ALPHA : GL_RED,
        (lumaFormat == GL_LUMINANCE_ALPHA) ? GL_ALPHA : GL_ZERO, GL_ZERO, GL_ZERO,
    };
    mFunctions->texParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);

    // Make a temporary framebuffer using the second scratch texture to render the swizzled result
    // to.
    mStateManager->bindTexture(gl::TextureType::_2D, mScratchTextures[1]);
    mFunctions->texImage2D(
        GL_TEXTURE_2D, 0, copyTexImageFormat.internalFormat, sourceArea.width, sourceArea.height, 0,
        gl::GetUnsizedFormat(copyTexImageFormat.internalFormat), readType, nullptr);

    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mScratchFBO);
    mFunctions->framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                     mScratchTextures[1], 0);

    // Render to the destination texture, sampling from the scratch texture
    ScopedGLState scopedState(mStateManager, mFunctions,
                              gl::Rectangle(0, 0, sourceArea.width, sourceArea.height));
    scopedState.willUseTextureUnit(0);

    setScratchTextureParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    setScratchTextureParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    mStateManager->activeTexture(0);
    mStateManager->bindTexture(gl::TextureType::_2D, mScratchTextures[0]);

    mStateManager->useProgram(blitProgram->program);
    mFunctions->uniform1i(blitProgram->sourceTextureLocation, 0);
    mFunctions->uniform2f(blitProgram->scaleLocation, 1.0, 1.0);
    mFunctions->uniform2f(blitProgram->offsetLocation, 0.0, 0.0);
    mFunctions->uniform1i(blitProgram->multiplyAlphaLocation, 0);
    mFunctions->uniform1i(blitProgram->unMultiplyAlphaLocation, 0);

    mStateManager->bindVertexArray(mVAO, 0);
    mFunctions->drawArrays(GL_TRIANGLES, 0, 3);

    // Copy the swizzled texture to the destination texture
    mStateManager->bindTexture(textureType, texture);

    if (target == gl::TextureTarget::_3D || target == gl::TextureTarget::_2DArray)
    {
        mFunctions->copyTexSubImage3D(ToGLenum(target), static_cast<GLint>(level), destOffset.x,
                                      destOffset.y, destOffset.z, 0, 0, sourceArea.width,
                                      sourceArea.height);
    }
    else
    {
        mFunctions->copyTexSubImage2D(ToGLenum(target), static_cast<GLint>(level), destOffset.x,
                                      destOffset.y, 0, 0, sourceArea.width, sourceArea.height);
    }

    // Finally orphan the scratch textures so they can be GCed by the driver.
    orphanScratchTextures();

    return gl::NoError();
}

gl::Error BlitGL::blitColorBufferWithShader(const gl::Framebuffer *source,
                                            const gl::Framebuffer *dest,
                                            const gl::Rectangle &sourceAreaIn,
                                            const gl::Rectangle &destAreaIn,
                                            GLenum filter)
{
    ANGLE_TRY(initializeResources());

    BlitProgram *blitProgram = nullptr;
    ANGLE_TRY(getBlitProgram(BlitProgramType::FLOAT_TO_FLOAT, &blitProgram));

    // We'll keep things simple by removing reversed coordinates from the rectangles. In the end
    // we'll apply the reversal to the source texture coordinates if needed. The destination
    // rectangle will be set to the gl viewport, which can't be reversed.
    bool reverseX            = sourceAreaIn.isReversedX() != destAreaIn.isReversedX();
    bool reverseY            = sourceAreaIn.isReversedY() != destAreaIn.isReversedY();
    gl::Rectangle sourceArea = sourceAreaIn.removeReversal();
    gl::Rectangle destArea   = destAreaIn.removeReversal();

    const gl::FramebufferAttachment *readAttachment = source->getReadColorbuffer();
    ASSERT(readAttachment->getSamples() <= 1);

    // Compute the part of the source that will be sampled.
    gl::Rectangle inBoundsSource;
    {
        gl::Extents sourceSize = readAttachment->getSize();
        gl::Rectangle sourceBounds(0, 0, sourceSize.width, sourceSize.height);
        if (!gl::ClipRectangle(sourceArea, sourceBounds, &inBoundsSource))
        {
            // Early out when the sampled part is empty as the blit will be a noop,
            // and it prevents a division by zero in later computations.
            return gl::NoError();
        }
    }

    // The blit will be emulated by getting the source of the blit in a texture and sampling it
    // with CLAMP_TO_EDGE.

    GLuint textureId;

    // TODO(cwallez) once texture dirty bits are landed, reuse attached texture instead of using
    // CopyTexImage2D
    {
        textureId = mScratchTextures[0];

        GLenum format                 = readAttachment->getFormat().info->internalFormat;
        const FramebufferGL *sourceGL = GetImplAs<FramebufferGL>(source);
        mStateManager->bindFramebuffer(GL_READ_FRAMEBUFFER, sourceGL->getFramebufferID());
        mStateManager->bindTexture(gl::TextureType::_2D, textureId);

        mFunctions->copyTexImage2D(GL_TEXTURE_2D, 0, format, inBoundsSource.x, inBoundsSource.y,
                                   inBoundsSource.width, inBoundsSource.height, 0);

        // Translate sourceArea to be relative to the copied image.
        sourceArea.x -= inBoundsSource.x;
        sourceArea.y -= inBoundsSource.y;

        setScratchTextureParameter(GL_TEXTURE_MIN_FILTER, filter);
        setScratchTextureParameter(GL_TEXTURE_MAG_FILTER, filter);
        setScratchTextureParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        setScratchTextureParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Transform the source area to the texture coordinate space (where 0.0 and 1.0 correspond to
    // the edges of the texture).
    Vector2 texCoordOffset(
        static_cast<float>(sourceArea.x) / static_cast<float>(inBoundsSource.width),
        static_cast<float>(sourceArea.y) / static_cast<float>(inBoundsSource.height));
    // texCoordScale is equal to the size of the source area in texture coordinates.
    Vector2 texCoordScale(
        static_cast<float>(sourceArea.width) / static_cast<float>(inBoundsSource.width),
        static_cast<float>(sourceArea.height) / static_cast<float>(inBoundsSource.height));

    if (reverseX)
    {
        texCoordOffset.x() = texCoordOffset.x() + texCoordScale.x();
        texCoordScale.x()  = -texCoordScale.x();
    }
    if (reverseY)
    {
        texCoordOffset.y() = texCoordOffset.y() + texCoordScale.y();
        texCoordScale.y()  = -texCoordScale.y();
    }

    // Reset all the state except scissor and use the viewport to draw exactly to the destination
    // rectangle
    ScopedGLState scopedState(mStateManager, mFunctions, destArea, ScopedGLState::KEEP_SCISSOR);
    scopedState.willUseTextureUnit(0);

    // Set uniforms
    mStateManager->activeTexture(0);
    mStateManager->bindTexture(gl::TextureType::_2D, textureId);

    mStateManager->useProgram(blitProgram->program);
    mFunctions->uniform1i(blitProgram->sourceTextureLocation, 0);
    mFunctions->uniform2f(blitProgram->scaleLocation, texCoordScale.x(), texCoordScale.y());
    mFunctions->uniform2f(blitProgram->offsetLocation, texCoordOffset.x(), texCoordOffset.y());
    mFunctions->uniform1i(blitProgram->multiplyAlphaLocation, 0);
    mFunctions->uniform1i(blitProgram->unMultiplyAlphaLocation, 0);

    const FramebufferGL *destGL = GetImplAs<FramebufferGL>(dest);
    mStateManager->bindFramebuffer(GL_DRAW_FRAMEBUFFER, destGL->getFramebufferID());

    mStateManager->bindVertexArray(mVAO, 0);
    mFunctions->drawArrays(GL_TRIANGLES, 0, 3);

    return gl::NoError();
}

gl::ErrorOrResult<bool> BlitGL::copySubTexture(const gl::Context *context,
                                               TextureGL *source,
                                               size_t sourceLevel,
                                               GLenum sourceComponentType,
                                               TextureGL *dest,
                                               gl::TextureTarget destTarget,
                                               size_t destLevel,
                                               GLenum destComponentType,
                                               const gl::Extents &sourceSize,
                                               const gl::Rectangle &sourceArea,
                                               const gl::Offset &destOffset,
                                               bool needsLumaWorkaround,
                                               GLenum lumaFormat,
                                               bool unpackFlipY,
                                               bool unpackPremultiplyAlpha,
                                               bool unpackUnmultiplyAlpha)
{
    ASSERT(source->getType() == gl::TextureType::_2D);
    ANGLE_TRY(initializeResources());

    // Make sure the destination texture can be rendered to before setting anything else up.  Some
    // cube maps may not be renderable until all faces have been filled.
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mScratchFBO);
    mFunctions->framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, ToGLenum(destTarget),
                                     dest->getTextureID(), static_cast<GLint>(destLevel));
    GLenum status = mFunctions->checkFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        return false;
    }

    BlitProgramType blitProgramType = getBlitProgramType(sourceComponentType, destComponentType);
    BlitProgram *blitProgram        = nullptr;
    ANGLE_TRY(getBlitProgram(blitProgramType, &blitProgram));

    // Setup the source texture
    if (needsLumaWorkaround)
    {
        GLint luminance = (lumaFormat == GL_ALPHA) ? GL_ZERO : GL_RED;

        GLint alpha = GL_RED;
        if (lumaFormat == GL_LUMINANCE)
        {
            alpha = GL_ONE;
        }
        else if (lumaFormat == GL_LUMINANCE_ALPHA)
        {
            alpha = GL_GREEN;
        }
        else
        {
            ASSERT(lumaFormat == GL_ALPHA);
        }

        GLint swizzle[4] = {luminance, luminance, luminance, alpha};
        source->setSwizzle(context, swizzle);
    }
    source->setMinFilter(context, GL_NEAREST);
    source->setMagFilter(context, GL_NEAREST);
    ANGLE_TRY(source->setBaseLevel(context, static_cast<GLuint>(sourceLevel)));

    // Render to the destination texture, sampling from the source texture
    ScopedGLState scopedState(
        mStateManager, mFunctions,
        gl::Rectangle(destOffset.x, destOffset.y, sourceArea.width, sourceArea.height));
    scopedState.willUseTextureUnit(0);

    mStateManager->activeTexture(0);
    mStateManager->bindTexture(gl::TextureType::_2D, source->getTextureID());

    Vector2 scale(sourceArea.width / static_cast<float>(sourceSize.width),
                  sourceArea.height / static_cast<float>(sourceSize.height));
    Vector2 offset(sourceArea.x / static_cast<float>(sourceSize.width),
                   sourceArea.y / static_cast<float>(sourceSize.height));
    if (unpackFlipY)
    {
        offset.y() += scale.y();
        scale.y() = -scale.y();
    }

    mStateManager->useProgram(blitProgram->program);
    mFunctions->uniform1i(blitProgram->sourceTextureLocation, 0);
    mFunctions->uniform2f(blitProgram->scaleLocation, scale.x(), scale.y());
    mFunctions->uniform2f(blitProgram->offsetLocation, offset.x(), offset.y());
    if (unpackPremultiplyAlpha == unpackUnmultiplyAlpha)
    {
        mFunctions->uniform1i(blitProgram->multiplyAlphaLocation, 0);
        mFunctions->uniform1i(blitProgram->unMultiplyAlphaLocation, 0);
    }
    else
    {
        mFunctions->uniform1i(blitProgram->multiplyAlphaLocation, unpackPremultiplyAlpha);
        mFunctions->uniform1i(blitProgram->unMultiplyAlphaLocation, unpackUnmultiplyAlpha);
    }

    mStateManager->bindVertexArray(mVAO, 0);
    mFunctions->drawArrays(GL_TRIANGLES, 0, 3);

    return true;
}

gl::Error BlitGL::copySubTextureCPUReadback(const gl::Context *context,
                                            TextureGL *source,
                                            size_t sourceLevel,
                                            GLenum sourceComponentType,
                                            TextureGL *dest,
                                            gl::TextureTarget destTarget,
                                            size_t destLevel,
                                            GLenum destFormat,
                                            GLenum destType,
                                            const gl::Rectangle &sourceArea,
                                            const gl::Offset &destOffset,
                                            bool unpackFlipY,
                                            bool unpackPremultiplyAlpha,
                                            bool unpackUnmultiplyAlpha)
{
    ANGLE_TRY(initializeResources());

    ASSERT(source->getType() == gl::TextureType::_2D);
    const auto &destInternalFormatInfo = gl::GetInternalFormatInfo(destFormat, destType);

    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mScratchFBO);
    mFunctions->framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                     source->getTextureID(), static_cast<GLint>(sourceLevel));
    GLenum status = mFunctions->checkFramebufferStatus(GL_FRAMEBUFFER);
    ASSERT(status == GL_FRAMEBUFFER_COMPLETE);

    // Create a buffer for holding the source and destination memory
    const size_t sourcePixelSize = 4;
    size_t sourceBufferSize      = sourceArea.width * sourceArea.height * sourcePixelSize;
    size_t destBufferSize =
        sourceArea.width * sourceArea.height * destInternalFormatInfo.pixelBytes;
    angle::MemoryBuffer *buffer = nullptr;
    ANGLE_TRY_ALLOCATION(context->getScratchBuffer(sourceBufferSize + destBufferSize, &buffer));

    uint8_t *sourceMemory = buffer->data();
    uint8_t *destMemory   = buffer->data() + sourceBufferSize;

    GLenum readPixelsFormat        = GL_NONE;
    ColorReadFunction readFunction = nullptr;
    if (sourceComponentType == GL_UNSIGNED_INT)
    {
        readPixelsFormat = GL_RGBA_INTEGER;
        readFunction     = angle::ReadColor<angle::R8G8B8A8, GLuint>;
    }
    else
    {
        ASSERT(sourceComponentType != GL_INT);
        readPixelsFormat = GL_RGBA;
        readFunction     = angle::ReadColor<angle::R8G8B8A8, GLfloat>;
    }

    gl::PixelUnpackState unpack;
    unpack.alignment = 1;
    mStateManager->setPixelUnpackState(unpack);
    mStateManager->setPixelUnpackBuffer(nullptr);
    mFunctions->readPixels(sourceArea.x, sourceArea.y, sourceArea.width, sourceArea.height,
                           readPixelsFormat, GL_UNSIGNED_BYTE, sourceMemory);

    angle::FormatID destFormatID =
        angle::Format::InternalFormatToID(destInternalFormatInfo.sizedInternalFormat);
    const auto &destFormatInfo = angle::Format::Get(destFormatID);
    CopyImageCHROMIUM(
        sourceMemory, sourceArea.width * sourcePixelSize, sourcePixelSize, readFunction, destMemory,
        sourceArea.width * destInternalFormatInfo.pixelBytes, destInternalFormatInfo.pixelBytes,
        destFormatInfo.colorWriteFunction, destInternalFormatInfo.format,
        destInternalFormatInfo.componentType, sourceArea.width, sourceArea.height, unpackFlipY,
        unpackPremultiplyAlpha, unpackUnmultiplyAlpha);

    gl::PixelPackState pack;
    pack.alignment = 1;
    mStateManager->setPixelPackState(pack);
    mStateManager->setPixelPackBuffer(nullptr);

    nativegl::TexSubImageFormat texSubImageFormat =
        nativegl::GetTexSubImageFormat(mFunctions, mWorkarounds, destFormat, destType);

    mStateManager->bindTexture(dest->getType(), dest->getTextureID());
    mFunctions->texSubImage2D(ToGLenum(destTarget), static_cast<GLint>(destLevel), destOffset.x,
                              destOffset.y, sourceArea.width, sourceArea.height,
                              texSubImageFormat.format, texSubImageFormat.type, destMemory);

    return gl::NoError();
}

gl::ErrorOrResult<bool> BlitGL::copyTexSubImage(TextureGL *source,
                                                size_t sourceLevel,
                                                TextureGL *dest,
                                                gl::TextureTarget destTarget,
                                                size_t destLevel,
                                                const gl::Rectangle &sourceArea,
                                                const gl::Offset &destOffset)
{
    ANGLE_TRY(initializeResources());

    // Make sure the source texture can create a complete framebuffer before continuing.
    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mScratchFBO);
    mFunctions->framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                     source->getTextureID(), static_cast<GLint>(sourceLevel));
    GLenum status = mFunctions->checkFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        return false;
    }

    mStateManager->bindTexture(dest->getType(), dest->getTextureID());

    mFunctions->copyTexSubImage2D(ToGLenum(destTarget), static_cast<GLint>(destLevel), destOffset.x,
                                  destOffset.y, sourceArea.x, sourceArea.y, sourceArea.width,
                                  sourceArea.height);

    return true;
}

gl::ErrorOrResult<bool> BlitGL::clearRenderableTexture(TextureGL *source,
                                                       GLenum sizedInternalFormat,
                                                       int numTextureLayers,
                                                       const gl::ImageIndex &imageIndex)
{
    ANGLE_TRY(initializeResources());

    ClearBindTargetVector bindTargets;
    GLbitfield clearMask = 0;
    ANGLE_TRY(PrepareForClear(mStateManager, sizedInternalFormat, &bindTargets, &clearMask));

    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mScratchFBO);

    if (nativegl::UseTexImage2D(source->getType()))
    {
        ASSERT(numTextureLayers == 1);
        for (GLenum bindTarget : bindTargets)
        {
            mFunctions->framebufferTexture2D(GL_FRAMEBUFFER, bindTarget,
                                             ToGLenum(imageIndex.getTarget()),
                                             source->getTextureID(), imageIndex.getLevelIndex());
        }

        GLenum status = mFunctions->checkFramebufferStatus(GL_FRAMEBUFFER);
        if (status == GL_FRAMEBUFFER_COMPLETE)
        {
            mFunctions->clear(clearMask);
        }
        else
        {
            UnbindAttachments(mFunctions, GL_FRAMEBUFFER, bindTargets);
            return false;
        }
    }
    else
    {
        ASSERT(nativegl::UseTexImage3D(source->getType()));

        // Check if it's possible to bind all layers of the texture at once
        if (mFunctions->framebufferTexture && !imageIndex.hasLayer())
        {
            for (GLenum bindTarget : bindTargets)
            {
                mFunctions->framebufferTexture(GL_FRAMEBUFFER, bindTarget, source->getTextureID(),
                                               imageIndex.getLevelIndex());
            }

            GLenum status = mFunctions->checkFramebufferStatus(GL_FRAMEBUFFER);
            if (status == GL_FRAMEBUFFER_COMPLETE)
            {
                mFunctions->clear(clearMask);
            }
            else
            {
                UnbindAttachments(mFunctions, GL_FRAMEBUFFER, bindTargets);
                return false;
            }
        }
        else
        {
            GLint firstLayer = 0;
            GLint layerCount = numTextureLayers;
            if (imageIndex.hasLayer())
            {
                firstLayer = imageIndex.getLayerIndex();
                layerCount = imageIndex.getLayerCount();
            }

            for (GLint layer = 0; layer < layerCount; layer++)
            {
                for (GLenum bindTarget : bindTargets)
                {
                    mFunctions->framebufferTextureLayer(
                        GL_FRAMEBUFFER, bindTarget, source->getTextureID(),
                        imageIndex.getLevelIndex(), layer + firstLayer);
                }

                GLenum status = mFunctions->checkFramebufferStatus(GL_FRAMEBUFFER);
                if (status == GL_FRAMEBUFFER_COMPLETE)
                {
                    mFunctions->clear(clearMask);
                }
                else
                {
                    UnbindAttachments(mFunctions, GL_FRAMEBUFFER, bindTargets);
                    return false;
                }
            }
        }
    }

    UnbindAttachments(mFunctions, GL_FRAMEBUFFER, bindTargets);
    return true;
}

gl::Error BlitGL::clearRenderbuffer(RenderbufferGL *source, GLenum sizedInternalFormat)
{
    ANGLE_TRY(initializeResources());

    ClearBindTargetVector bindTargets;
    GLbitfield clearMask = 0;
    ANGLE_TRY(PrepareForClear(mStateManager, sizedInternalFormat, &bindTargets, &clearMask));

    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, mScratchFBO);
    for (GLenum bindTarget : bindTargets)
    {
        mFunctions->framebufferRenderbuffer(GL_FRAMEBUFFER, bindTarget, GL_RENDERBUFFER,
                                            source->getRenderbufferID());
    }
    mFunctions->clear(clearMask);

    // Unbind
    for (GLenum bindTarget : bindTargets)
    {
        mFunctions->framebufferRenderbuffer(GL_FRAMEBUFFER, bindTarget, GL_RENDERBUFFER, 0);
    }

    return gl::NoError();
}

gl::Error BlitGL::clearFramebuffer(FramebufferGL *source)
{
    // initializeResources skipped because no local state is used

    // Clear all attachments
    GLbitfield clearMask = 0;
    ANGLE_TRY(SetClearState(mStateManager, true, true, true, &clearMask));

    mStateManager->bindFramebuffer(GL_FRAMEBUFFER, source->getFramebufferID());
    mFunctions->clear(clearMask);

    return gl::NoError();
}

gl::Error BlitGL::initializeResources()
{
    for (size_t i = 0; i < ArraySize(mScratchTextures); i++)
    {
        if (mScratchTextures[i] == 0)
        {
            mFunctions->genTextures(1, &mScratchTextures[i]);
        }
    }

    if (mScratchFBO == 0)
    {
        mFunctions->genFramebuffers(1, &mScratchFBO);
    }

    if (mVertexBuffer == 0)
    {
        mFunctions->genBuffers(1, &mVertexBuffer);
        mStateManager->bindBuffer(gl::BufferBinding::Array, mVertexBuffer);

        // Use a single, large triangle, to avoid arithmetic precision issues where fragments
        // with the same Y coordinate don't get exactly the same interpolated texcoord Y.
        float vertexData[] = {
            -0.5f, 0.0f, 1.5f, 0.0f, 0.5f, 2.0f,
        };

        mFunctions->bufferData(GL_ARRAY_BUFFER, sizeof(float) * 6, vertexData, GL_STATIC_DRAW);
    }

    if (mVAO == 0)
    {
        mFunctions->genVertexArrays(1, &mVAO);

        mStateManager->bindVertexArray(mVAO, 0);
        mStateManager->bindBuffer(gl::BufferBinding::Array, mVertexBuffer);

        // Enable all attributes with the same buffer so that it doesn't matter what location the
        // texcoord attribute is assigned
        GLint maxAttributes = 0;
        mFunctions->getIntegerv(GL_MAX_VERTEX_ATTRIBS, &maxAttributes);

        for (GLint i = 0; i < maxAttributes; i++)
        {
            mFunctions->enableVertexAttribArray(i);
            mFunctions->vertexAttribPointer(i, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        }
    }

    return gl::NoError();
}

void BlitGL::orphanScratchTextures()
{
    for (auto texture : mScratchTextures)
    {
        mStateManager->bindTexture(gl::TextureType::_2D, texture);
        gl::PixelUnpackState unpack;
        mStateManager->setPixelUnpackState(unpack);
        mStateManager->setPixelUnpackBuffer(nullptr);
        mFunctions->texImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                               nullptr);
    }
}

void BlitGL::setScratchTextureParameter(GLenum param, GLenum value)
{
    for (auto texture : mScratchTextures)
    {
        mStateManager->bindTexture(gl::TextureType::_2D, texture);
        mFunctions->texParameteri(GL_TEXTURE_2D, param, value);
        mFunctions->texParameteri(GL_TEXTURE_2D, param, value);
    }
}

BlitGL::BlitProgramType BlitGL::getBlitProgramType(GLenum sourceComponentType,
                                                   GLenum destComponentType)
{
    if (sourceComponentType == GL_UNSIGNED_INT)
    {
        ASSERT(destComponentType == GL_UNSIGNED_INT);
        return BlitProgramType::UINT_TO_UINT;
    }
    else
    {
        // Source is a float type
        ASSERT(sourceComponentType != GL_INT);
        if (destComponentType == GL_UNSIGNED_INT)
        {
            return BlitProgramType::FLOAT_TO_UINT;
        }
        else
        {
            // Dest is a float type
            return BlitProgramType::FLOAT_TO_FLOAT;
        }
    }
}

gl::Error BlitGL::getBlitProgram(BlitProgramType type, BlitProgram **program)
{
    BlitProgram &result = mBlitPrograms[type];
    if (result.program == 0)
    {
        result.program = mFunctions->createProgram();

        // Depending on what types need to be output by the shaders, different versions need to be
        // used.
        std::string version;
        std::string vsInputVariableQualifier;
        std::string vsOutputVariableQualifier;
        std::string fsInputVariableQualifier;
        std::string fsOutputVariableQualifier;
        std::string sampleFunction;
        if (type == BlitProgramType::FLOAT_TO_FLOAT)
        {
            version                   = "100";
            vsInputVariableQualifier  = "attribute";
            vsOutputVariableQualifier = "varying";
            fsInputVariableQualifier  = "varying";
            fsOutputVariableQualifier = "";
            sampleFunction            = "texture2D";
        }
        else
        {
            // Need to use a higher version to support non-float output types
            if (mFunctions->standard == STANDARD_GL_DESKTOP)
            {
                version = "330";
            }
            else
            {
                ASSERT(mFunctions->standard == STANDARD_GL_ES);
                version = "300 es";
            }
            vsInputVariableQualifier  = "in";
            vsOutputVariableQualifier = "out";
            fsInputVariableQualifier  = "in";
            fsOutputVariableQualifier = "out";
            sampleFunction            = "texture";
        }

        {
            // Compile the vertex shader
            std::ostringstream vsSourceStream;
            vsSourceStream << "#version " << version << "\n";
            vsSourceStream << vsInputVariableQualifier << " vec2 a_texcoord;\n";
            vsSourceStream << "uniform vec2 u_scale;\n";
            vsSourceStream << "uniform vec2 u_offset;\n";
            vsSourceStream << vsOutputVariableQualifier << " vec2 v_texcoord;\n";
            vsSourceStream << "\n";
            vsSourceStream << "void main()\n";
            vsSourceStream << "{\n";
            vsSourceStream << "    gl_Position = vec4((a_texcoord * 2.0) - 1.0, 0.0, 1.0);\n";
            vsSourceStream << "    v_texcoord = a_texcoord * u_scale + u_offset;\n";
            vsSourceStream << "}\n";

            std::string vsSourceStr  = vsSourceStream.str();
            const char *vsSourceCStr = vsSourceStr.c_str();

            GLuint vs = mFunctions->createShader(GL_VERTEX_SHADER);
            mFunctions->shaderSource(vs, 1, &vsSourceCStr, nullptr);
            mFunctions->compileShader(vs);
            ANGLE_TRY(CheckCompileStatus(mFunctions, vs));

            mFunctions->attachShader(result.program, vs);
            mFunctions->deleteShader(vs);
        }

        {
            // Sampling texture uniform changes depending on source texture type.
            std::string samplerType;
            std::string samplerResultType;
            switch (type)
            {
                case BlitProgramType::FLOAT_TO_FLOAT:
                case BlitProgramType::FLOAT_TO_UINT:
                    samplerType       = "sampler2D";
                    samplerResultType = "vec4";
                    break;

                case BlitProgramType::UINT_TO_UINT:
                    samplerType       = "usampler2D";
                    samplerResultType = "uvec4";
                    break;

                default:
                    UNREACHABLE();
                    break;
            }

            // Output variables depend on the output type
            std::string outputType;
            std::string outputVariableName;
            std::string outputMultiplier;
            switch (type)
            {
                case BlitProgramType::FLOAT_TO_FLOAT:
                    outputType         = "";
                    outputVariableName = "gl_FragColor";
                    outputMultiplier   = "1.0";
                    break;

                case BlitProgramType::FLOAT_TO_UINT:
                case BlitProgramType::UINT_TO_UINT:
                    outputType         = "uvec4";
                    outputVariableName = "outputUint";
                    outputMultiplier   = "255.0";
                    break;

                default:
                    UNREACHABLE();
                    break;
            }

            // Compile the fragment shader
            std::ostringstream fsSourceStream;
            fsSourceStream << "#version " << version << "\n";
            fsSourceStream << "precision highp float;\n";
            fsSourceStream << "uniform " << samplerType << " u_source_texture;\n";

            // Write the rest of the uniforms and varyings
            fsSourceStream << "uniform bool u_multiply_alpha;\n";
            fsSourceStream << "uniform bool u_unmultiply_alpha;\n";
            fsSourceStream << fsInputVariableQualifier << " vec2 v_texcoord;\n";
            if (!outputType.empty())
            {
                fsSourceStream << fsOutputVariableQualifier << " " << outputType << " "
                               << outputVariableName << ";\n";
            }

            // Write the main body
            fsSourceStream << "\n";
            fsSourceStream << "void main()\n";
            fsSourceStream << "{\n";

            // discard if the texcoord is outside (0, 1)^2 so the blitframebuffer workaround
            // doesn't write when the point sampled is outside of the source framebuffer.
            fsSourceStream << "    if (clamp(v_texcoord, vec2(0.0), vec2(1.0)) != v_texcoord)\n";
            fsSourceStream << "    {\n";
            fsSourceStream << "        discard;\n";
            fsSourceStream << "    }\n";

            // Sampling code depends on the input data type
            fsSourceStream << "    " << samplerResultType << " color = " << sampleFunction
                           << "(u_source_texture, v_texcoord);\n";

            // Perform the premultiply or unmultiply alpha logic
            fsSourceStream << "    if (u_multiply_alpha)\n";
            fsSourceStream << "    {\n";
            fsSourceStream << "        color.xyz = color.xyz * color.a;\n";
            fsSourceStream << "    }\n";
            fsSourceStream << "    if (u_unmultiply_alpha && color.a != 0.0)\n";
            fsSourceStream << "    {\n";
            fsSourceStream << "         color.xyz = color.xyz / color.a;\n";
            fsSourceStream << "    }\n";

            // Write the conversion to the destionation type
            fsSourceStream << "    color = color * " << outputMultiplier << ";\n";

            // Write the output assignment code
            fsSourceStream << "    " << outputVariableName << " = " << outputType << "(color);\n";
            fsSourceStream << "}\n";

            std::string fsSourceStr  = fsSourceStream.str();
            const char *fsSourceCStr = fsSourceStr.c_str();

            GLuint fs = mFunctions->createShader(GL_FRAGMENT_SHADER);
            mFunctions->shaderSource(fs, 1, &fsSourceCStr, nullptr);
            mFunctions->compileShader(fs);
            ANGLE_TRY(CheckCompileStatus(mFunctions, fs));

            mFunctions->attachShader(result.program, fs);
            mFunctions->deleteShader(fs);
        }

        mFunctions->linkProgram(result.program);
        ANGLE_TRY(CheckLinkStatus(mFunctions, result.program));

        result.sourceTextureLocation =
            mFunctions->getUniformLocation(result.program, "u_source_texture");
        result.scaleLocation  = mFunctions->getUniformLocation(result.program, "u_scale");
        result.offsetLocation = mFunctions->getUniformLocation(result.program, "u_offset");
        result.multiplyAlphaLocation =
            mFunctions->getUniformLocation(result.program, "u_multiply_alpha");
        result.unMultiplyAlphaLocation =
            mFunctions->getUniformLocation(result.program, "u_unmultiply_alpha");
    }

    *program = &result;
    return gl::NoError();
}

}  // namespace rx
