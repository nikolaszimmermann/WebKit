/*
 * Copyright (C) 2013 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "RenderTextLineBoxes.h"

#include "LegacyInlineTextBox.h"
#include "LegacyRootInlineBox.h"
#include "RenderBlock.h"
#include "RenderStyleInlines.h"
#include "RenderSVGInlineText.h"
#include "RenderView.h"
#include "VisiblePosition.h"

namespace WebCore {

RenderTextLineBoxes::RenderTextLineBoxes()
{
}

LegacyInlineTextBox* RenderTextLineBoxes::createAndAppendLineBox(RenderSVGInlineText& renderText)
{
    auto textBox = renderText.createTextBox();
    if (!m_first) {
        m_first = textBox.get();
        m_last = textBox.get();
    } else {
        m_last->setNextTextBox(textBox.get());
        textBox->setPreviousTextBox(m_last);
        m_last = textBox.get();
    }
    return textBox.release();
}

void RenderTextLineBoxes::remove(LegacyInlineTextBox& box)
{
    checkConsistency();

    if (&box == m_first)
        m_first = box.nextTextBox();
    if (&box == m_last)
        m_last = box.prevTextBox();
    if (box.nextTextBox())
        box.nextTextBox()->setPreviousTextBox(box.prevTextBox());
    if (box.prevTextBox())
        box.prevTextBox()->setNextTextBox(box.nextTextBox());

    checkConsistency();
}

void RenderTextLineBoxes::removeAllFromParent(RenderSVGInlineText& renderer)
{
    if (!m_first) {
        if (renderer.parent())
            renderer.parent()->dirtyLineFromChangedChild();
        return;
    }
    for (auto* box = m_first; box; box = box->nextTextBox())
        box->removeFromParent();
}

void RenderTextLineBoxes::deleteAll()
{
    if (!m_first)
        return;
    LegacyInlineTextBox* next;
    for (auto* current = m_first; current; current = next) {
        next = current->nextTextBox();
        delete current;
    }
    m_first = nullptr;
    m_last = nullptr;
}

void RenderTextLineBoxes::dirtyForTextChange(RenderSVGInlineText& renderer)
{
    for (auto* box = m_first; box; box = box->nextTextBox())
        box->dirtyLineBoxes();

    if (!m_first && renderer.parent())
        renderer.parent()->dirtyLineFromChangedChild();
}

inline void RenderTextLineBoxes::checkConsistency() const
{
#if ASSERT_ENABLED
#ifdef CHECK_CONSISTENCY
    const LegacyInlineTextBox* prev = nullptr;
    for (auto* child = m_first; child; child = child->nextTextBox()) {
        ASSERT(child->renderer() == this);
        ASSERT(child->prevTextBox() == prev);
        prev = child;
    }
    ASSERT(prev == m_last);
#endif
#endif // ASSERT_ENABLED
}

#if ASSERT_ENABLED
RenderTextLineBoxes::~RenderTextLineBoxes()
{
    ASSERT(!m_first);
    ASSERT(!m_last);
}
#endif

#if !ASSERT_WITH_SECURITY_IMPLICATION_DISABLED
void RenderTextLineBoxes::invalidateParentChildLists()
{
    for (auto* box = m_first; box; box = box->nextTextBox())
        box->invalidateParentChildList();
}
#endif

}
