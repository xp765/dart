/*
 * Copyright (c) 2011-2018, The DART development contributors
 * All rights reserved.
 *
 * The list of contributors can be found at:
 *   https://github.com/dartsim/dart/blob/master/LICENSE
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "dart/gui/filament/ImGuiHelper.hpp"

#include <vector>
#include <unordered_map>

#include "dart/external/imgui/imgui.h"

//#include <filamat/MaterialBuilder.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/Texture.h>
#include <filament/View.h>
#include <filament/TransformManager.h>
#include <utils/EntityManager.h>

namespace dart {
namespace gui {
namespace flmt {

namespace filagui {

static const uint8_t UI_BLIT_PACKAGE[] = {
    #include "generated/material/uiBlit.inc"
};

ImGuiHelper::ImGuiHelper(
    filament::Engine* engine, filament::View* view, const utils::Path& fontPath)
  : mEngine(engine), mView(view)
{
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();

  // If the given font path is invalid, ImGui will silently fall back to proggy,
  // which is a
  // tiny "pixel art" texture that is compiled into the library.
  // TODO(JS): commented
  // io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f);

  // Create the grayscale texture that ImGui uses for its glyph atlas.
  static unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsAlpha8(&pixels, &width, &height);
  size_t size = (size_t)(width * height);
  filament::Texture::PixelBufferDescriptor pb(
      pixels, size, filament::Texture::Format::R, filament::Texture::Type::UBYTE);
  mTexture = filament::Texture::Builder()
                 .width((uint32_t)width)
                 .height((uint32_t)height)
                 .levels((uint8_t)1)
                 .format(filament::Texture::InternalFormat::R8)
                 .sampler(filament::Texture::Sampler::SAMPLER_2D)
                 .build(*engine);
  mTexture->setImage(*engine, 0, std::move(pb));

  // Create a simple alpha-blended 2D blitting material.
  filament::Material* material
      = filament::Material::Builder()
            .package((void*)UI_BLIT_PACKAGE, sizeof(UI_BLIT_PACKAGE))
            .build(*engine);

  filament::TextureSampler sampler(
      filament::TextureSampler::MinFilter::LINEAR, filament::TextureSampler::MagFilter::LINEAR);
  material->setDefaultParameter("albedo", mTexture, sampler);
  mMaterial = material;

  // Create a scene solely for our one and only Renderable.
  filament::Scene* scene = engine->createScene();
  view->setScene(scene);
  ::utils::EntityManager& em = ::utils::EntityManager::get();
  mRenderable = em.create();
  scene->addEntity(mRenderable);

  ImGui::StyleColorsDark();
}

ImGuiHelper::~ImGuiHelper()
{
  mEngine->destroy(mRenderable);
  for (auto& mi : mMaterialInstances)
  {
    mEngine->destroy(mi);
  }
  mEngine->destroy(mMaterial);
  mEngine->destroy(mTexture);
  for (auto& vb : mVertexBuffers)
  {
    mEngine->destroy(vb);
  }
  for (auto& ib : mIndexBuffers)
  {
    mEngine->destroy(ib);
  }
  ImGui::DestroyContext();
}

void ImGuiHelper::setDisplaySize(
    int width, int height, float scaleX, float scaleY)
{
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(width, height);
  io.DisplayFramebufferScale.x = scaleX;
  io.DisplayFramebufferScale.y = scaleY;
}

void ImGuiHelper::render(float timeStepInSeconds, Callback imguiCommands)
{
  ImGuiIO& io = ImGui::GetIO();
  io.DeltaTime = timeStepInSeconds;
  // First, let ImGui process events and increment its internal frame count.
  // This call will also update the io.WantCaptureMouse, io.WantCaptureKeyboard
  // flag
  // that tells us whether to dispatch inputs (or not) to the app.
  ImGui::NewFrame();
  // Allow the client app to create widgets.
  imguiCommands(mEngine, mView);
  // Let ImGui build up its draw data.
  ImGui::Render();
  // Finally, translate the draw data into Filament objects.
  renderDrawData(ImGui::GetDrawData());
}

// To help with mapping unique scissor rectangles to material instances, we
// create a 64-bit
// key from a 4-tuple that defines an AABB in screen space.
static uint64_t makeScissorKey(int fbheight, const ImVec4& clipRect)
{
  uint16_t left = (uint16_t)clipRect.x;
  uint16_t bottom = (uint16_t)(fbheight - clipRect.w);
  uint16_t width = (uint16_t)(clipRect.z - clipRect.x);
  uint16_t height = (uint16_t)(clipRect.w - clipRect.y);
  return ((uint64_t)left << 0ull) | ((uint64_t)bottom << 16ull)
         | ((uint64_t)width << 32ull) | ((uint64_t)height << 48ull);
}

void ImGuiHelper::renderDrawData(ImDrawData* imguiData)
{
  mHasSynced = false;
  auto& rcm = mEngine->getRenderableManager();

  // Avoid rendering when minimized and scale coordinates for retina displays.
  ImGuiIO& io = ImGui::GetIO();
  int fbwidth = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x);
  int fbheight = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
  if (fbwidth == 0 || fbheight == 0)
    return;
  imguiData->ScaleClipRects(io.DisplayFramebufferScale);

  // Ensure that we have enough vertex buffers and index buffers.
  createBuffers(imguiData->CmdListsCount);

  // Count how many primitives we'll need, then create a Renderable builder.
  // Also count how many unique scissor rectangles are required.
  size_t nPrims = 0;
  std::unordered_map<uint64_t, filament::MaterialInstance*> scissorRects;
  for (int cmdListIndex = 0; cmdListIndex < imguiData->CmdListsCount;
       cmdListIndex++)
  {
    const ImDrawList* cmds = imguiData->CmdLists[cmdListIndex];
    nPrims += cmds->CmdBuffer.size();
    for (const auto& pcmd : cmds->CmdBuffer)
    {
      scissorRects[makeScissorKey(fbheight, pcmd.ClipRect)] = nullptr;
    }
  }
  auto rbuilder = filament::RenderableManager::Builder(nPrims);
  rbuilder.boundingBox({{0, 0, 0}, {10000, 10000, 10000}}).culling(false);

  // Ensure that we have a material instance for each scissor rectangle.
  size_t previousSize = mMaterialInstances.size();
  if (scissorRects.size() > mMaterialInstances.size())
  {
    mMaterialInstances.resize(scissorRects.size());
    for (size_t i = previousSize; i < mMaterialInstances.size(); i++)
    {
      // TODO(JS): commented
      assert(mMaterial);
      mMaterialInstances[i] = mMaterial->createInstance();
    }
  }

  // Push each unique scissor rectangle to a MaterialInstance.
  size_t matIndex = 0;
  for (auto& pair : scissorRects)
  {
    pair.second = mMaterialInstances[matIndex++];
    uint32_t left = (pair.first >> 0ull) & 0xffffull;
    uint32_t bottom = (pair.first >> 16ull) & 0xffffull;
    uint32_t width = (pair.first >> 32ull) & 0xffffull;
    uint32_t height = (pair.first >> 48ull) & 0xffffull;
    pair.second->setScissor(left, bottom, width, height);
  }

  // Recreate the Renderable component and point it to the vertex buffers.
  rcm.destroy(mRenderable);
  int bufferIndex = 0;
  int primIndex = 0;
  for (int cmdListIndex = 0; cmdListIndex < imguiData->CmdListsCount;
       cmdListIndex++)
  {
    const ImDrawList* cmds = imguiData->CmdLists[cmdListIndex];
    size_t indexOffset = 0;
    populateVertexData(
        bufferIndex,
        cmds->VtxBuffer.Size * sizeof(ImDrawVert),
        cmds->VtxBuffer.Data,
        cmds->IdxBuffer.Size * sizeof(ImDrawIdx),
        cmds->IdxBuffer.Data);
    for (const auto& pcmd : cmds->CmdBuffer)
    {
      const size_t capacity = mIndexBuffers[bufferIndex]->getIndexCount();
      const size_t required = indexOffset + pcmd.ElemCount;
      assert(required <= capacity);
      if (pcmd.UserCallback)
      {
        pcmd.UserCallback(cmds, &pcmd);
      }
      else
      {
        uint64_t skey = makeScissorKey(fbheight, pcmd.ClipRect);
        auto miter = scissorRects.find(skey);
        assert(miter != scissorRects.end());
        rbuilder
            .geometry(
                primIndex,
                filament::RenderableManager::PrimitiveType::TRIANGLES,
                mVertexBuffers[bufferIndex],
                mIndexBuffers[bufferIndex],
                indexOffset,
                pcmd.ElemCount)
            .blendOrder(primIndex, primIndex)
            .material(primIndex, miter->second);
        primIndex++;
      }
      indexOffset += pcmd.ElemCount;
    }
    bufferIndex++;
  }
  if (imguiData->CmdListsCount > 0)
  {
    rbuilder.build(*mEngine, mRenderable);
  }
}

void ImGuiHelper::createVertexBuffer(size_t bufferIndex, size_t capacity)
{
  syncThreads();
  mEngine->destroy(mVertexBuffers[bufferIndex]);
  mVertexBuffers[bufferIndex] = filament::VertexBuffer::Builder()
                                    .vertexCount(capacity)
                                    .bufferCount(1)
                                    .attribute(
                                        filament::VertexAttribute::POSITION,
                                        0,
                                        filament::VertexBuffer::AttributeType::FLOAT2,
                                        0,
                                        sizeof(ImDrawVert))
                                    .attribute(
                                        filament::VertexAttribute::UV0,
                                        0,
                                        filament::VertexBuffer::AttributeType::FLOAT2,
                                        sizeof(math::float2),
                                        sizeof(ImDrawVert))
                                    .attribute(
                                        filament::VertexAttribute::COLOR,
                                        0,
                                        filament::VertexBuffer::AttributeType::UBYTE4,
                                        2 * sizeof(math::float2),
                                        sizeof(ImDrawVert))
                                    .normalized(filament::VertexAttribute::COLOR)
                                    .build(*mEngine);
}

void ImGuiHelper::createIndexBuffer(size_t bufferIndex, size_t capacity)
{
  syncThreads();
  mEngine->destroy(mIndexBuffers[bufferIndex]);
  mIndexBuffers[bufferIndex] = filament::IndexBuffer::Builder()
                                   .indexCount(capacity)
                                   .bufferType(filament::IndexBuffer::IndexType::USHORT)
                                   .build(*mEngine);
}

void ImGuiHelper::createBuffers(int numRequiredBuffers)
{
  if (numRequiredBuffers > mVertexBuffers.size())
  {
    size_t previousSize = mVertexBuffers.size();
    mVertexBuffers.resize(numRequiredBuffers, nullptr);
    for (size_t i = previousSize; i < mVertexBuffers.size(); i++)
    {
      // Pick a reasonable starting capacity; it will grow if needed.
      createVertexBuffer(i, 1000);
    }
  }
  if (numRequiredBuffers > mIndexBuffers.size())
  {
    size_t previousSize = mIndexBuffers.size();
    mIndexBuffers.resize(numRequiredBuffers, nullptr);
    for (size_t i = previousSize; i < mIndexBuffers.size(); i++)
    {
      // Pick a reasonable starting capacity; it will grow if needed.
      createIndexBuffer(i, 5000);
    }
  }
}

void ImGuiHelper::populateVertexData(
    size_t bufferIndex,
    size_t vbSizeInBytes,
    void* vbImguiData,
    size_t ibSizeInBytes,
    void* ibImguiData)
{
  // Create a new vertex buffer if the size isn't large enough, then copy the
  // ImGui data into
  // a staging area since Filament's render thread might consume the data at any
  // time.
  size_t requiredVertCount = vbSizeInBytes / sizeof(ImDrawVert);
  size_t capacityVertCount = mVertexBuffers[bufferIndex]->getVertexCount();
  if (requiredVertCount > capacityVertCount)
  {
    createVertexBuffer(bufferIndex, requiredVertCount);
  }
  size_t nVbBytes = requiredVertCount * sizeof(ImDrawVert);
  void* vbFilamentData = malloc(nVbBytes);
  memcpy(vbFilamentData, vbImguiData, nVbBytes);
  mVertexBuffers[bufferIndex]->setBufferAt(
      *mEngine,
      0,
      filament::VertexBuffer::BufferDescriptor(
          vbFilamentData,
          nVbBytes,
          (filament::VertexBuffer::BufferDescriptor::Callback)free));

  // Create a new index buffer if the size isn't large enough, then copy the
  // ImGui data into
  // a staging area since Filament's render thread might consume the data at any
  // time.
  size_t requiredIndexCount = ibSizeInBytes / 2;
  size_t capacityIndexCount = mIndexBuffers[bufferIndex]->getIndexCount();
  if (requiredIndexCount > capacityIndexCount)
  {
    createIndexBuffer(bufferIndex, requiredIndexCount);
  }
  size_t nIbBytes = requiredIndexCount * 2;
  void* ibFilamentData = malloc(nIbBytes);
  memcpy(ibFilamentData, ibImguiData, nIbBytes);
  mIndexBuffers[bufferIndex]->setBuffer(
      *mEngine,
      filament::IndexBuffer::BufferDescriptor(
          ibFilamentData,
          nIbBytes,
          (filament::IndexBuffer::BufferDescriptor::Callback)free));
}

void ImGuiHelper::syncThreads()
{
  if (!mHasSynced)
  {
    // This is called only when ImGui needs to grow a vertex buffer, which
    // occurs a few times
    // after launching and rarely (if ever) after that.
    filament::Fence::waitAndDestroy(mEngine->createFence());
    mHasSynced = true;
  }
}

} // namespace filagui

} // namespace flmt
} // namespace gui
} // namespace dart