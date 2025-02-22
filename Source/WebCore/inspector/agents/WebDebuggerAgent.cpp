/*
 * Copyright (C) 2014-2022 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebDebuggerAgent.h"

#include "EventListener.h"
#include "EventTarget.h"
#include "InstrumentingAgents.h"
#include "ScriptExecutionContext.h"
#include "Timer.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

using namespace Inspector;

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebDebuggerAgent);

WebDebuggerAgent::WebDebuggerAgent(WebAgentContext& context)
    : InspectorDebuggerAgent(context)
    , m_instrumentingAgents(context.instrumentingAgents)
{
}

WebDebuggerAgent::~WebDebuggerAgent() = default;

bool WebDebuggerAgent::enabled() const
{
    return m_instrumentingAgents.enabledWebDebuggerAgent() == this && InspectorDebuggerAgent::enabled();
}

void WebDebuggerAgent::internalEnable()
{
    m_instrumentingAgents.setEnabledWebDebuggerAgent(this);

    InspectorDebuggerAgent::internalEnable();
}

void WebDebuggerAgent::internalDisable(bool isBeingDestroyed)
{
    m_instrumentingAgents.setEnabledWebDebuggerAgent(nullptr);

    InspectorDebuggerAgent::internalDisable(isBeingDestroyed);
}

void WebDebuggerAgent::didAddEventListener(EventTarget& target, const AtomString& eventType, EventListener& listener, bool capture)
{
    if (!breakpointsActive())
        return;

    auto& eventListeners = target.eventListeners(eventType);
    auto position = eventListeners.findIf([&](auto& registeredListener) {
        return &registeredListener->callback() == &listener && registeredListener->useCapture() == capture;
    });
    if (position == notFound)
        return;

    auto& registeredListener = eventListeners.at(position);
    if (m_registeredEventListeners.contains(registeredListener.get()))
        return;

    auto* globalObject = target.scriptExecutionContext()->globalObject();
    if (!globalObject)
        return;

    int identifier = m_nextEventListenerIdentifier++;
    m_registeredEventListeners.set(registeredListener.get(), identifier);

    didScheduleAsyncCall(globalObject, InspectorDebuggerAgent::AsyncCallType::EventListener, identifier, registeredListener->isOnce());
}

void WebDebuggerAgent::willRemoveEventListener(EventTarget& target, const AtomString& eventType, EventListener& listener, bool capture)
{
    auto& eventListeners = target.eventListeners(eventType);
    size_t listenerIndex = eventListeners.findIf([&](auto& registeredListener) {
        return &registeredListener->callback() == &listener && registeredListener->useCapture() == capture;
    });

    if (listenerIndex == notFound)
        return;

    int identifier = m_registeredEventListeners.take(eventListeners[listenerIndex].get());
    didCancelAsyncCall(InspectorDebuggerAgent::AsyncCallType::EventListener, identifier);
}

void WebDebuggerAgent::willHandleEvent(const RegisteredEventListener& listener)
{
    auto it = m_registeredEventListeners.find(&listener);
    if (it == m_registeredEventListeners.end())
        return;

    // Save the identifier for the listener we're about to dispatch an event to in case it's removed.
    m_dispatchedEventListeners.set(&listener, it->value);

    willDispatchAsyncCall(InspectorDebuggerAgent::AsyncCallType::EventListener, it->value);
}

void WebDebuggerAgent::didHandleEvent(const RegisteredEventListener& listener)
{
    auto it = m_dispatchedEventListeners.find(&listener);
    if (it == m_dispatchedEventListeners.end())
        return;

    didDispatchAsyncCall(InspectorDebuggerAgent::AsyncCallType::EventListener, it->value);

    m_dispatchedEventListeners.remove(it);
}

int WebDebuggerAgent::willPostMessage()
{
    if (!breakpointsActive())
        return 0;

    auto postMessageIdentifier = m_nextPostMessageIdentifier++;
    m_postMessageTasks.add(postMessageIdentifier);
    return postMessageIdentifier;
}

void WebDebuggerAgent::didPostMessage(int postMessageIdentifier, JSC::JSGlobalObject& state)
{
    if (!breakpointsActive())
        return;

    if (!postMessageIdentifier || !m_postMessageTasks.contains(postMessageIdentifier))
        return;

    didScheduleAsyncCall(&state, InspectorDebuggerAgent::AsyncCallType::PostMessage, postMessageIdentifier, true);
}

void WebDebuggerAgent::didFailPostMessage(int postMessageIdentifier)
{
    if (!postMessageIdentifier)
        return;

    auto it = m_postMessageTasks.find(postMessageIdentifier);
    if (it == m_postMessageTasks.end())
        return;

    didCancelAsyncCall(InspectorDebuggerAgent::AsyncCallType::PostMessage, postMessageIdentifier);

    m_postMessageTasks.remove(it);
}

void WebDebuggerAgent::willDispatchPostMessage(int postMessageIdentifier)
{
    if (!postMessageIdentifier || !m_postMessageTasks.contains(postMessageIdentifier))
        return;

    willDispatchAsyncCall(InspectorDebuggerAgent::AsyncCallType::PostMessage, postMessageIdentifier);
}

void WebDebuggerAgent::didDispatchPostMessage(int postMessageIdentifier)
{
    if (!postMessageIdentifier)
        return;

    auto it = m_postMessageTasks.find(postMessageIdentifier);
    if (it == m_postMessageTasks.end())
        return;

    didDispatchAsyncCall(InspectorDebuggerAgent::AsyncCallType::PostMessage, postMessageIdentifier);

    m_postMessageTasks.remove(it);
}

void WebDebuggerAgent::didRequestAnimationFrame(int callbackId, JSC::JSGlobalObject& state)
{
    if (!breakpointsActive())
        return;

    didScheduleAsyncCall(&state, InspectorDebuggerAgent::AsyncCallType::RequestAnimationFrame, callbackId, true);
}

void WebDebuggerAgent::willFireAnimationFrame(int callbackId)
{
    willDispatchAsyncCall(InspectorDebuggerAgent::AsyncCallType::RequestAnimationFrame, callbackId);
}

void WebDebuggerAgent::didCancelAnimationFrame(int callbackId)
{
    didCancelAsyncCall(InspectorDebuggerAgent::AsyncCallType::RequestAnimationFrame, callbackId);
}

void WebDebuggerAgent::didFireAnimationFrame(int callbackId)
{
    didDispatchAsyncCall(InspectorDebuggerAgent::AsyncCallType::RequestAnimationFrame, callbackId);
}

void WebDebuggerAgent::didClearAsyncStackTraceData()
{
    InspectorDebuggerAgent::didClearAsyncStackTraceData();

    m_registeredEventListeners.clear();
    m_dispatchedEventListeners.clear();
    m_postMessageTasks.clear();
    m_nextEventListenerIdentifier = 1;
    m_nextPostMessageIdentifier = 1;
}

} // namespace WebCore
