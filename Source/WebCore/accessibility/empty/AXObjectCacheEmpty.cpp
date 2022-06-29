/*
 * Copyright (C) 2022 Microsoft Corporation.
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
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#if ENABLE(ACCESSIBILITY) && !USE(ATK) && !USE(ATSPI)

#include "AXObjectCache.h"

namespace WebCore {

void AXObjectCache::detachWrapper(AXCoreObject* obj, AccessibilityDetachmentType) { }
void AXObjectCache::attachWrapper(AXCoreObject*) { }
void AXObjectCache::handleScrolledToAnchor(const Node* anchorNode) { }
void AXObjectCache::postPlatformNotification(AXCoreObject* obj, AXNotification notification) { }
void AXObjectCache::nodeTextChangePlatformNotification(AccessibilityObject*, AXTextChange, unsigned, const String&){ }
void AXObjectCache::frameLoadingEventPlatformNotification(AccessibilityObject* obj, AXLoadingEvent notification) { }
void AXObjectCache::platformHandleFocusedUIElementChanged(Node*, Node* newFocusedNode) { }
void AXObjectCache::platformPerformDeferredCacheUpdate() { }

} // namespace WebCore

#endif // ENABLE(ACCESSIBILITY)
