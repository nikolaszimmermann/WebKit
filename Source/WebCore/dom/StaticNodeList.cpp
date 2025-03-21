/*
 * Copyright (C) 2007, 2010 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "StaticNodeList.h"

#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_OR_ISO_ALLOCATED_IMPL(StaticNodeList);
WTF_MAKE_TZONE_OR_ISO_ALLOCATED_IMPL(StaticWrapperNodeList);
WTF_MAKE_TZONE_OR_ISO_ALLOCATED_IMPL(StaticElementList);

StaticNodeList::StaticNodeList(Vector<Ref<Node>>&& nodes)
    : m_nodes(WTFMove(nodes))
{
}

unsigned StaticNodeList::length() const
{
    return m_nodes.size();
}

Node* StaticNodeList::item(unsigned index) const
{
    if (index < m_nodes.size())
        return const_cast<Node*>(m_nodes[index].ptr());
    return nullptr;
}

unsigned StaticWrapperNodeList::length() const
{
    return m_nodeList->length();
}

Node* StaticWrapperNodeList::item(unsigned index) const
{
    return m_nodeList->item(index);
}

unsigned StaticElementList::length() const
{
    return m_elements.size();
}

Element* StaticElementList::item(unsigned index) const
{
    if (index < m_elements.size())
        return const_cast<Element*>(m_elements[index].ptr());
    return nullptr;
}

} // namespace WebCore
