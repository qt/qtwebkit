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

#include "config.h"
#include "NetworkingContext.h"
#include "ResourceRequest.h"
#include "ThirdPartyCookiesQt.h"

#if ENABLE(BLOB)
#include "BlobData.h"
#include "BlobRegistryImpl.h"
#include "BlobStorageData.h"
#endif

#include <QNetworkRequest>
#include <QUrl>
#include <wtf/text/Base64.h>

namespace WebCore {

// The limit can be found in qhttpnetworkconnection.cpp.
// To achieve the best result we want WebKit to schedule the jobs so we
// are using the limit as found in Qt. To allow Qt to fill its queue
// and prepare jobs we will schedule two more downloads.
// Per TCP connection there is 1 current processed, 3 possibly pipelined
// and 2 ready to re-fill the pipeline.
unsigned initializeMaximumHTTPConnectionCountPerHost()
{
    return 6 * (1 + 3 + 2);
}

#if ENABLE(BLOB)
static bool appendBlobResolved(Vector<char>& out, const KURL& url, QString* contentType = 0)
{
    RefPtr<BlobStorageData> blobData = static_cast<BlobRegistryImpl&>(blobRegistry()).getBlobDataFromURL(url);
    if (!blobData)
        return false;

    if (contentType)
        *contentType = blobData->contentType();

    BlobDataItemList::const_iterator it = blobData->items().begin();
    const BlobDataItemList::const_iterator itend = blobData->items().end();
    for (; it != itend; ++it) {
        const BlobDataItem& blobItem = *it;
        if (blobItem.type == BlobDataItem::Data) {
            if (!out.tryAppend(reinterpret_cast<const char*>(blobItem.data->data()) + blobItem.offset, blobItem.length))
                return false;
        } else if (blobItem.type == BlobDataItem::File) {
            // File types are not allowed here, so just ignore it.
        } else
            ASSERT_NOT_REACHED();
    }
    return true;
}

static QUrl resolveBlobUrl(const KURL& url)
{
    RefPtr<BlobStorageData> blobData = static_cast<BlobRegistryImpl&>(blobRegistry()).getBlobDataFromURL(url);
    if (!blobData)
        return QUrl();

    Vector<char> data;
    QString contentType;
    if (!appendBlobResolved(data, url, &contentType)) {
        qWarning("Failed to convert blob data to base64: cannot allocate memory for continuous blob data");
        return QUrl();
    }

    // QByteArray::{from,to}Base64 are prone to integer overflow, this is the maximum size that can be safe
    size_t maxBase64Size = std::numeric_limits<int>::max() / 3 - 1;

    Vector<char> base64;
    WTF::base64Encode(data, base64, WTF::Base64URLPolicy);
    if (base64.isEmpty() || base64.size() > maxBase64Size) {
        qWarning("Failed to convert blob data to base64: data is too large");
        return QUrl();
    }

    QString dataUri(QStringLiteral("data:"));
    dataUri.append(contentType);
    dataUri.append(QStringLiteral(";base64,"));
    dataUri.reserve(dataUri.size() + base64.size());
    dataUri.append(QLatin1String(base64.data(), base64.size()));
    return QUrl(dataUri);
}

QUrl convertBlobToDataUrl(const QUrl& url)
{
    QT_TRY {
        return resolveBlobUrl(url);
    } QT_CATCH(const std::bad_alloc &) {
        qWarning("Failed to convert blob data to base64: not enough memory");
    }
    return QUrl();
}
#endif

static QUrl toQUrl(const KURL& url)
{
#if ENABLE(BLOB)
    if (url.protocolIs("blob"))
        return convertBlobToDataUrl(url);
#endif
    return url;
}

static inline QByteArray stringToByteArray(const String& string)
{
    if (string.is8Bit())
        return QByteArray(reinterpret_cast<const char*>(string.characters8()), string.length());
    return QString(string).toLatin1();
}

QNetworkRequest ResourceRequest::toNetworkRequest(NetworkingContext *context) const
{
    QNetworkRequest request;
    QUrl newurl = toQUrl(url());
    request.setUrl(newurl);
    request.setOriginatingObject(context ? context->originatingObject() : 0);

    const HTTPHeaderMap &headers = httpHeaderFields();
    for (HTTPHeaderMap::const_iterator it = headers.begin(), end = headers.end();
         it != end; ++it) {
        QByteArray name = stringToByteArray(it->key);
        // QNetworkRequest::setRawHeader() would remove the header if the value is null
        // Make sure to set an empty header instead of null header.
        if (!it->value.isNull())
            request.setRawHeader(name, stringToByteArray(it->value));
        else
            request.setRawHeader(name, QByteArrayLiteral(""));
    }

    // Make sure we always have an Accept header; some sites require this to
    // serve subresources
    if (!request.hasRawHeader("Accept"))
        request.setRawHeader("Accept", "*/*");

    switch (cachePolicy()) {
    case ReloadIgnoringCacheData:
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        break;
    case ReturnCacheDataElseLoad:
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
        break;
    case ReturnCacheDataDontLoad:
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysCache);
        break;
    case UseProtocolCachePolicy:
        // QNetworkRequest::PreferNetwork
    default:
        break;
    }

    if (!allowCookies() || !thirdPartyCookiePolicyPermits(context, url(), firstPartyForCookies())) {
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    }

    if (!allowCookies())
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);

    return request;
}

}

