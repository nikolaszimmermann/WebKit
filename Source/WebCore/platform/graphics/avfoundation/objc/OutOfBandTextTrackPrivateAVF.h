/*
 * Copyright (C) 2014-2025 Apple Inc. All rights reserved.
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

#ifndef OutOfBandTextTrackPrivateAVF_h
#define OutOfBandTextTrackPrivateAVF_h

#if ENABLE(VIDEO) && (USE(AVFOUNDATION) || PLATFORM(IOS_FAMILY))

#include "InbandTextTrackPrivateAVF.h"
#include <wtf/TZoneMallocInlines.h>

OBJC_CLASS AVMediaSelectionOption;

namespace WebCore {
    
class OutOfBandTextTrackPrivateAVF : public InbandTextTrackPrivateAVF {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(OutOfBandTextTrackPrivateAVF);
public:
    static Ref<OutOfBandTextTrackPrivateAVF> create(AVMediaSelectionOption* selection, TrackID trackID, ModeChangedCallback&& callback)
    {
        return adoptRef(*new OutOfBandTextTrackPrivateAVF(selection, trackID, WTFMove(callback)));
    }
    
    void processCue(CFArrayRef, CFArrayRef, const MediaTime&) override { }
    void resetCueValues() override { }
    
    Category textTrackCategory() const override { return OutOfBand; }
    
    AVMediaSelectionOption* mediaSelectionOption() const { return m_mediaSelectionOption.get(); }
    
protected:
    OutOfBandTextTrackPrivateAVF(AVMediaSelectionOption* selection, TrackID trackID, ModeChangedCallback&& callback)
        : InbandTextTrackPrivateAVF(trackID, InbandTextTrackPrivate::CueFormat::Generic, WTFMove(callback))
        , m_mediaSelectionOption(selection)
    {
    }

    bool isOutOfBandTextTrackPrivateAVF() const final { return true; }
    
    RetainPtr<AVMediaSelectionOption> m_mediaSelectionOption;
};
    
}

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::OutOfBandTextTrackPrivateAVF)
static bool isType(const WebCore::InbandTextTrackPrivateAVF& track) { return track.isOutOfBandTextTrackPrivateAVF(); }
SPECIALIZE_TYPE_TRAITS_END()

#endif

#endif // OutOfBandTextTrackPrivateAVF_h
