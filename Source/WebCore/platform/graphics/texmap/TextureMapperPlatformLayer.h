/*
    Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#pragma once

#if USE(TEXTURE_MAPPER)

#include "TransformationMatrix.h"

namespace WebCore {

class Color;
class TextureMapper;

class TextureMapperPlatformLayer {
public:
    class Client {
    public:
        virtual void platformLayerWillBeDestroyed() = 0;
        virtual void setPlatformLayerNeedsDisplay() = 0;
    };

    TextureMapperPlatformLayer() = default;
    virtual ~TextureMapperPlatformLayer() = default;

    virtual void paintToTextureMapper(TextureMapper&, const FloatRect&, const TransformationMatrix& modelViewMatrix = TransformationMatrix(), float opacity = 1.0) = 0;

    WEBCORE_EXPORT virtual void drawBorder(TextureMapper&, const Color&, float borderWidth, const FloatRect&, const TransformationMatrix&);

    void setClient(TextureMapperPlatformLayer::Client* client) { m_client = client; }

    virtual bool isHolePunchBuffer() const { return false; }
    virtual void notifyVideoPosition(const FloatRect&, const TransformationMatrix&) { };
    virtual void paintTransparentRectangle(TextureMapper&, const FloatRect&, const TransformationMatrix&) { };

protected:
    TextureMapperPlatformLayer::Client* client() { return m_client; }

private:
    TextureMapperPlatformLayer::Client* m_client { nullptr };
};

} // namespace WebCore

#endif // USE(TEXTURE_MAPPER)
