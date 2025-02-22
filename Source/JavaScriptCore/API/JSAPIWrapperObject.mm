/*
 * Copyright (C) 2013-2022 Apple Inc. All rights reserved.
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

#import "config.h"
#import "JSAPIWrapperObject.h"

#import "JSCInlines.h"
#import "JSCallbackObject.h"
#import "JSVirtualMachineInternal.h"
#import "Structure.h"
#import <wtf/NeverDestroyed.h>

#if JSC_OBJC_API_ENABLED

class JSAPIWrapperObjectHandleOwner final : public JSC::WeakHandleOwner {
public:
    void finalize(JSC::Handle<JSC::Unknown>, void*) final;
    bool isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown>, void* context, JSC::AbstractSlotVisitor&, ASCIILiteral*) final;
};

static JSAPIWrapperObjectHandleOwner* jsAPIWrapperObjectHandleOwner()
{
    static NeverDestroyed<JSAPIWrapperObjectHandleOwner> jsWrapperObjectHandleOwner;
    return &jsWrapperObjectHandleOwner.get();
}

void JSAPIWrapperObjectHandleOwner::finalize(JSC::Handle<JSC::Unknown> handle, void*)
{
    JSC::JSAPIWrapperObject* wrapperObject = static_cast<JSC::JSAPIWrapperObject*>(handle.get().asCell());
    if (!wrapperObject->wrappedObject())
        return;

    JSC::Heap::heap(wrapperObject)->releaseSoon(adoptNS(static_cast<id>(wrapperObject->wrappedObject())));
    JSC::WeakSet::deallocate(JSC::WeakImpl::asWeakImpl(handle.slot()));
}

bool JSAPIWrapperObjectHandleOwner::isReachableFromOpaqueRoots(JSC::Handle<JSC::Unknown> handle, void*, JSC::AbstractSlotVisitor& visitor, ASCIILiteral*)
{
    JSC::JSAPIWrapperObject* wrapperObject = JSC::jsCast<JSC::JSAPIWrapperObject*>(handle.get().asCell());
    // We use the JSGlobalObject when processing weak handles to prevent the situation where using
    // the same Objective-C object in multiple global objects keeps all of the global objects alive.
    if (!wrapperObject->wrappedObject())
        return false;
    return visitor.isMarked(wrapperObject->structure()->globalObject()) && visitor.containsOpaqueRoot(wrapperObject->wrappedObject());
}

namespace JSC {

static JSC_DECLARE_HOST_FUNCTION(callJSAPIWrapperObjectCallbackObject);
static JSC_DECLARE_HOST_FUNCTION(constructJSAPIWrapperObjectCallbackObject);
static JSC_DECLARE_CUSTOM_GETTER(callbackGetterJSAPIWrapperObjectCallbackObject);
static JSC_DECLARE_CUSTOM_GETTER(staticFunctionGetterJSAPIWrapperObjectCallbackObject);

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(template<>, JSCallbackObject<JSAPIWrapperObject>);

template <> const ClassInfo JSCallbackObject<JSAPIWrapperObject>::s_info = { "JSAPIWrapperObject"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(JSCallbackObject) };
template <> const JSC::DestructionMode JSCallbackObject<JSAPIWrapperObject>::needsDestruction = JSC::NeedsDestruction;

template <>
NativeFunction::Ptr JSCallbackObject<JSAPIWrapperObject>::getCallFunction()
{
    return callJSAPIWrapperObjectCallbackObject;
}

template <>
NativeFunction::Ptr JSCallbackObject<JSAPIWrapperObject>::getConstructFunction()
{
    return constructJSAPIWrapperObjectCallbackObject;
}

template <>
GetValueFunc JSCallbackObject<JSAPIWrapperObject>::getCallbackGetter()
{
    return callbackGetterJSAPIWrapperObjectCallbackObject;
}

template <>
GetValueFunc JSCallbackObject<JSAPIWrapperObject>::getStaticFunctionGetter()
{
    return staticFunctionGetterJSAPIWrapperObjectCallbackObject;
}

template <>
Structure* JSCallbackObject<JSAPIWrapperObject>::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue proto)
{
    return Structure::create(vm, globalObject, proto, TypeInfo(ObjectType, StructureFlags), &s_info);
}

JSAPIWrapperObject::JSAPIWrapperObject(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

void JSAPIWrapperObject::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    WeakSet::allocate(this, jsAPIWrapperObjectHandleOwner(), 0); // Balanced in JSAPIWrapperObjectHandleOwner::finalize.
}
    
void JSAPIWrapperObject::setWrappedObject(void* wrappedObject)
{
    ASSERT(!m_wrappedObject);
    m_wrappedObject = [static_cast<id>(wrappedObject) retain];
}

template<typename Visitor>
void JSAPIWrapperObject::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSAPIWrapperObject* thisObject = JSC::jsCast<JSAPIWrapperObject*>(cell);
    Base::visitChildren(cell, visitor);

    void* wrappedObject = thisObject->wrappedObject();
    if (wrappedObject)
        scanExternalObjectGraph(visitor.vm(), visitor, wrappedObject);
}

DEFINE_VISIT_CHILDREN_WITH_MODIFIER(JS_EXPORT_PRIVATE, JSAPIWrapperObject);

template <>
GCClient::IsoSubspace* JSCallbackObject<JSAPIWrapperObject>::subspaceForImpl(VM& vm, SubspaceAccess mode)
{
    switch (mode) {
    case SubspaceAccess::OnMainThread:
        return vm.apiWrapperObjectSpace<SubspaceAccess::OnMainThread>();
    case SubspaceAccess::Concurrently:
        return vm.apiWrapperObjectSpace<SubspaceAccess::Concurrently>();
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

JSC_DEFINE_HOST_FUNCTION(callJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSCallbackObject<JSAPIWrapperObject>::callImpl(globalObject, callFrame);
}

JSC_DEFINE_HOST_FUNCTION(constructJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    return JSCallbackObject<JSAPIWrapperObject>::constructImpl(globalObject, callFrame);
}

JSC_DEFINE_CUSTOM_GETTER(callbackGetterJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    return JSCallbackObject<JSAPIWrapperObject>::callbackGetterImpl(globalObject, thisValue, propertyName);
}

JSC_DEFINE_CUSTOM_GETTER(staticFunctionGetterJSAPIWrapperObjectCallbackObject, (JSGlobalObject* globalObject, EncodedJSValue thisValue, PropertyName propertyName))
{
    return JSCallbackObject<JSAPIWrapperObject>::staticFunctionGetterImpl(globalObject, thisValue, propertyName);
}

} // namespace JSC

#endif // JSC_OBJC_API_ENABLED
