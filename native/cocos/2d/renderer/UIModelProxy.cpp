/****************************************************************************
 Copyright (c) 2019-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "UIModelProxy.h"
#include <algorithm>
#include "2d/renderer/RenderEntity.h"
#include "base/Utils.h"
#include "core/assets/RenderingSubMesh.h"

namespace cc {
namespace {
constexpr uint32_t MIN_VERTEX_CAPACITY = 64;
constexpr uint32_t MAX_VERTEX_CAPACITY = 65535;
constexpr uint32_t MIN_INDEX_CAPACITY = 128;
constexpr uint32_t MAX_INDEX_CAPACITY = 65535 * 2;

// Buffers grow to the next power of two that fits the drawn geometry: no 2.25 MB
// reservation per Graphics up front and no new GPU buffer on every size change.
uint32_t graphicsBufferCapacity(uint32_t count, uint32_t minimum, uint32_t maximum) {
    return std::min(maximum, utils::nextPOT(std::max(count, minimum)));
}
} // namespace

UIModelProxy::UIModelProxy() {
    _device = Root::getInstance()->getDevice();
}

UIModelProxy::~UIModelProxy() {
    destroy();
}

void UIModelProxy::initModel(Node* node) {
    _model = Root::getInstance()->createModel<scene::Model>();
    _model->setNode(node);
    _model->setTransform(node);
    _node = node;
}

void UIModelProxy::activeSubModels() {
    if (_model == nullptr) return;
    auto* entity = static_cast<RenderEntity*>(_node->getUserData());
    auto drawInfoSize = entity->getDynamicRenderDrawInfos().size();
    auto subModelSize = _model->getSubModels().size();
    if (drawInfoSize > subModelSize) {
        for (size_t i = subModelSize; i < drawInfoSize; i++) {
            if (_model->getSubModels().size() <= i) {
                RenderDrawInfo* drawInfo = entity->getDynamicRenderDrawInfo(static_cast<uint32_t>(i));
                if (drawInfo == nullptr) {
                    return;
                }

                const uint32_t vertexCapacity = graphicsBufferCapacity(drawInfo->getVertexOffset(), MIN_VERTEX_CAPACITY, MAX_VERTEX_CAPACITY);
                const uint32_t indexCapacity = graphicsBufferCapacity(drawInfo->getIndexOffset(), MIN_INDEX_CAPACITY, MAX_INDEX_CAPACITY);
                auto* vertexBuffer = _device->createBuffer({
                    gfx::BufferUsageBit::VERTEX | gfx::BufferUsageBit::TRANSFER_DST,
                    gfx::MemoryUsageBit::DEVICE,
                    vertexCapacity * _stride,
                    _stride,
                });
                auto* indexBuffer = _device->createBuffer({
                    gfx::BufferUsageBit::INDEX | gfx::BufferUsageBit::TRANSFER_DST,
                    gfx::MemoryUsageBit::DEVICE,
                    static_cast<uint32_t>(indexCapacity * sizeof(uint16_t)),
                    sizeof(uint16_t),
                });
                gfx::BufferList vbReference;
                vbReference.emplace_back(vertexBuffer);

                auto* renderMesh = ccnew RenderingSubMesh(vbReference, _attributes, _primitiveMode, indexBuffer);
                renderMesh->setSubMeshIdx(0);

                _model->initSubModel(static_cast<index_t>(i), renderMesh, drawInfo->getMaterial());
                _graphicsUseSubMeshes.emplace_back(renderMesh);
            }
        }
    }
}

void UIModelProxy::uploadData() {
    auto* entity = static_cast<RenderEntity*>(_node->getUserData());
    const auto& drawInfos = entity->getDynamicRenderDrawInfos();
    const auto& subModelList = _model->getSubModels();
    for (size_t i = 0; i < drawInfos.size(); i++) {
        auto* drawInfo = drawInfos[i];
        auto* ia = subModelList.at(i)->getInputAssembler();
        if (drawInfo->getVertexOffset() <= 0 || drawInfo->getIndexOffset() <= 0) continue;
        gfx::BufferList vBuffers = ia->getVertexBuffers();
        if (!vBuffers.empty()) {
            const uint32_t vertexBytes = drawInfo->getVertexOffset() * _stride;
            if (vertexBytes > vBuffers[0]->getSize()) {
                vBuffers[0]->resize(graphicsBufferCapacity(drawInfo->getVertexOffset(), MIN_VERTEX_CAPACITY, MAX_VERTEX_CAPACITY) * _stride);
            }
            vBuffers[0]->update(drawInfo->getVDataBuffer(), vertexBytes); // vdata
        }
        ia->setVertexCount(drawInfo->getVertexOffset()); // count

        gfx::Buffer* iBuffer = ia->getIndexBuffer();
        const auto indexBytes = static_cast<uint32_t>(drawInfo->getIndexOffset() * sizeof(uint16_t));
        if (indexBytes > iBuffer->getSize()) {
            iBuffer->resize(static_cast<uint32_t>(graphicsBufferCapacity(drawInfo->getIndexOffset(), MIN_INDEX_CAPACITY, MAX_INDEX_CAPACITY) * sizeof(uint16_t)));
        }
        iBuffer->update(drawInfo->getIDataBuffer(), indexBytes); // idata
        ia->setIndexCount(drawInfo->getIndexOffset());           // indexCount
        // drawInfo->setModel(_model); // hack, render by model
    }

    if (!drawInfos.empty()) {
        drawInfos[0]->setModel(_model);
    }
}

void UIModelProxy::destroy() {
    if (_model != nullptr) {
        Root::getInstance()->destroyModel(_model);
        _model = nullptr;
    }

    for (auto& subMesh : _graphicsUseSubMeshes) {
        subMesh->destroy();
        subMesh = nullptr;
    }
    _graphicsUseSubMeshes.clear();

    _models.clear();
}

void UIModelProxy::clear() {
    if (_model == nullptr) {
        return;
    }
    const auto& subModels = _model->getSubModels();
    for (const auto &subModel : subModels) {
        auto *ia = subModel->getInputAssembler();
        ia->setVertexCount(0);
        ia->setIndexCount(0);
    }
}

// for ui model
void UIModelProxy::updateModels(scene::Model* model) {
    _models.emplace_back(model);
}

void UIModelProxy::attachDrawInfo() {
    auto* entity = static_cast<RenderEntity*>(_node->getUserData());
    auto& drawInfos = entity->getDynamicRenderDrawInfos();
    if (drawInfos.size() != _models.size()) return;
    for (size_t i = 0; i < drawInfos.size(); i++) {
        drawInfos[i]->setModel(_models[i]);
    }
}

void UIModelProxy::attachNode(Node* node) {
    _node = node;
}

void UIModelProxy::clearModels() {
    _models.clear();
}

} // namespace cc
