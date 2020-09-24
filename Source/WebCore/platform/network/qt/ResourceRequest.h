/*
 * Copyright (C) 2003, 2006 Apple Computer, Inc.  All rights reserved.
 * Copyright (C) 2006 Samuel Weinig <sam.weinig@gmail.com>
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#ifndef ResourceRequest_h
#define ResourceRequest_h

#include "ResourceRequestBase.h"

// HTTP/2 is implemented since Qt 5.8, but various QtNetwork bugs make it unusable in browser with Qt < 5.10.1
// We also don't enable HTTP/2 for unencrypted connections because of possible compatibility issues; it can be
// enabled manually by user application via custom QNAM subclass
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 1) && !defined(QT_NO_SSL)
#define USE_HTTP2 1
#endif

QT_BEGIN_NAMESPACE
class QNetworkRequest;
QT_END_NAMESPACE

namespace WebCore {
class NetworkingContext;

    class ResourceRequest : public ResourceRequestBase {
    public:
        ResourceRequest(const String& url) 
            : ResourceRequestBase(URL(ParsedURLString, url), UseProtocolCachePolicy)
        {
        }

        ResourceRequest(const URL& url) 
            : ResourceRequestBase(url, UseProtocolCachePolicy)
        {
        }

        ResourceRequest(const URL& url, const String& referrer, ResourceRequestCachePolicy policy = UseProtocolCachePolicy) 
            : ResourceRequestBase(url, policy)
        {
            setHTTPReferrer(referrer);
        }

        ResourceRequest()
            : ResourceRequestBase(URL(), UseProtocolCachePolicy)
        {
        }

        void updateFromDelegatePreservingOldProperties(const ResourceRequest& delegateProvidedRequest) { *this = delegateProvidedRequest; }

        QNetworkRequest toNetworkRequest(NetworkingContext* = 0) const;

#if USE(HTTP2)
        // Don't enable HTTP/2 when ALPN support status is unknown
        static bool alpnIsSupported();
#endif


    private:
        friend class ResourceRequestBase;

        void doUpdatePlatformRequest() { }
        void doUpdateResourceRequest() { }
        void doUpdatePlatformHTTPBody() { }
        void doUpdateResourceHTTPBody() { }

        std::unique_ptr<CrossThreadResourceRequestData> doPlatformCopyData(std::unique_ptr<CrossThreadResourceRequestData> data) const { return data; }
        void doPlatformAdopt(std::unique_ptr<CrossThreadResourceRequestData>) { }
    };

    struct CrossThreadResourceRequestData : public CrossThreadResourceRequestDataBase {
    };

} // namespace WebCore

#endif // ResourceRequest_h
