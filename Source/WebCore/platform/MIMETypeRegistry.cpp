/*
 * Copyright (C) 2006, 2008, 2009 Apple Inc.  All rights reserved.
 * Copyright (C) 2008, 2009 Torch Mobile Inc. All rights reserved. (http://www.torchmobile.com/)
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
#include "MIMETypeRegistry.h"

#include "MediaPlayer.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringHash.h>

#if USE(CG)
#include "ImageSourceCG.h"
#if !PLATFORM(IOS)
#include <ApplicationServices/ApplicationServices.h>
#else
#include <ImageIO/CGImageDestination.h>
#endif
#include <wtf/RetainPtr.h>
#endif

#if PLATFORM(QT)
#include <QImageReader>
#include <QImageWriter>
#endif

#if ENABLE(WEB_ARCHIVE) || ENABLE(MHTML)
#include "ArchiveFactory.h"
#endif

namespace WebCore {

namespace {
struct TypeExtensionPair {
    const char* type;
    const char* extension;
};
}

// A table of common media MIME types and file extenstions used when a platform's
// specific MIME type lookup doesn't have a match for a media file extension.
static const TypeExtensionPair commonMediaTypes[] = {

    // Ogg
    { "application/ogg", "ogx" },
    { "audio/ogg", "ogg" },
    { "audio/ogg", "oga" },
    { "video/ogg", "ogv" },

    // Annodex
    { "application/annodex", "anx" },
    { "audio/annodex", "axa" },
    { "video/annodex", "axv" },
    { "audio/speex", "spx" },

    // WebM
    { "video/webm", "webm" },
    { "audio/webm", "webm" },

    // MPEG
    { "audio/mpeg", "m1a" },
    { "audio/mpeg", "m2a" },
    { "audio/mpeg", "m1s" },
    { "audio/mpeg", "mpa" },
    { "video/mpeg", "mpg" },
    { "video/mpeg", "m15" },
    { "video/mpeg", "m1s" },
    { "video/mpeg", "m1v" },
    { "video/mpeg", "m75" },
    { "video/mpeg", "mpa" },
    { "video/mpeg", "mpeg" },
    { "video/mpeg", "mpm" },
    { "video/mpeg", "mpv" },

    // MPEG playlist
    { "application/vnd.apple.mpegurl", "m3u8" },
    { "application/mpegurl", "m3u8" },
    { "application/x-mpegurl", "m3u8" },
    { "audio/mpegurl", "m3url" },
    { "audio/x-mpegurl", "m3url" },
    { "audio/mpegurl", "m3u" },
    { "audio/x-mpegurl", "m3u" },

    // MPEG-4
    { "video/x-m4v", "m4v" },
    { "audio/x-m4a", "m4a" },
    { "audio/x-m4b", "m4b" },
    { "audio/x-m4p", "m4p" },
    { "audio/mp4", "m4a" },

    // MP3
    { "audio/mp3", "mp3" },
    { "audio/x-mp3", "mp3" },
    { "audio/x-mpeg", "mp3" },

    // MPEG-2
    { "video/x-mpeg2", "mp2" },
    { "video/mpeg2", "vob" },
    { "video/mpeg2", "mod" },
    { "video/m2ts", "m2ts" },
    { "video/x-m2ts", "m2t" },
    { "video/x-m2ts", "ts" },

    // 3GP/3GP2
    { "audio/3gpp", "3gpp" }, 
    { "audio/3gpp2", "3g2" }, 
    { "application/x-mpeg", "amc" },

    // AAC
    { "audio/aac", "aac" },
    { "audio/aac", "adts" },
    { "audio/x-aac", "m4r" },

    // CoreAudio File
    { "audio/x-caf", "caf" },
    { "audio/x-gsm", "gsm" },

    // ADPCM
    { "audio/x-wav", "wav" }
};

static HashSet<String, ASCIICaseInsensitiveHash>* supportedImageResourceMIMETypes;
static HashSet<String, ASCIICaseInsensitiveHash>* supportedImageMIMETypes;
static HashSet<String, ASCIICaseInsensitiveHash>* supportedImageMIMETypesForEncoding;
static HashSet<String, ASCIICaseInsensitiveHash>* supportedJavaScriptMIMETypes;
static HashSet<String, ASCIICaseInsensitiveHash>* supportedNonImageMIMETypes;
static HashSet<String, ASCIICaseInsensitiveHash>* supportedMediaMIMETypes;
static HashSet<String, ASCIICaseInsensitiveHash>* pdfMIMETypes;
static HashSet<String, ASCIICaseInsensitiveHash>* pdfAndPostScriptMIMETypes;
static HashSet<String, ASCIICaseInsensitiveHash>* unsupportedTextMIMETypes;

typedef HashMap<String, Vector<String>*, ASCIICaseInsensitiveHash> MediaMIMETypeMap;
    
static void initializeSupportedImageMIMETypes()
{
#if USE(CG)
    RetainPtr<CFArrayRef> supportedTypes = adoptCF(CGImageSourceCopyTypeIdentifiers());
    CFIndex count = CFArrayGetCount(supportedTypes.get());
    for (CFIndex i = 0; i < count; i++) {
        CFStringRef supportedType = reinterpret_cast<CFStringRef>(CFArrayGetValueAtIndex(supportedTypes.get(), i));
        String mimeType = MIMETypeForImageSourceType(supportedType);
        if (!mimeType.isEmpty()) {
            supportedImageMIMETypes->add(mimeType);
            supportedImageResourceMIMETypes->add(mimeType);
        }
    }

    // On Tiger and Leopard, com.microsoft.bmp doesn't have a MIME type in the registry.
    supportedImageMIMETypes->add("image/bmp");
    supportedImageResourceMIMETypes->add("image/bmp");

    // Favicons don't have a MIME type in the registry either.
    supportedImageMIMETypes->add("image/vnd.microsoft.icon");
    supportedImageMIMETypes->add("image/x-icon");
    supportedImageResourceMIMETypes->add("image/vnd.microsoft.icon");
    supportedImageResourceMIMETypes->add("image/x-icon");

    //  We only get one MIME type per UTI, hence our need to add these manually
    supportedImageMIMETypes->add("image/pjpeg");
    supportedImageResourceMIMETypes->add("image/pjpeg");

    //  We don't want to try to treat all binary data as an image
    supportedImageMIMETypes->remove("application/octet-stream");
    supportedImageResourceMIMETypes->remove("application/octet-stream");

    //  Don't treat pdf/postscript as images directly
    supportedImageMIMETypes->remove("application/pdf");
    supportedImageMIMETypes->remove("application/postscript");

#if PLATFORM(IOS)
    // Add malformed image mimetype for compatibility with Mail and to handle malformed mimetypes from the net
    // These were removed for <rdar://problem/6564538> Re-enable UTI code in WebCore now that MobileCoreServices exists
    // But Mail relies on at least image/tif reported as being supported (should be image/tiff).
    // This can be removed when Mail addresses:
    // <rdar://problem/7879510> Mail should use standard image mimetypes 
    // and we fix sniffing so that it corrects items such as image/jpg -> image/jpeg.
    static const char* malformedMIMETypes[] = {
        // JPEG (image/jpeg)
        "image/jpg", "image/jp_", "image/jpe_", "application/jpg", "application/x-jpg", "image/pipeg",
        "image/vnd.switfview-jpeg", "image/x-xbitmap",
        // GIF (image/gif)
        "image/gi_",
        // PNG (image/png)
        "application/png", "application/x-png",
        // TIFF (image/tiff)
        "image/x-tif", "image/tif", "image/x-tiff", "application/tif", "application/x-tif", "application/tiff",
        "application/x-tiff",
        // BMP (image/bmp, image/x-bitmap)
        "image/x-bmp", "image/x-win-bitmap", "image/x-windows-bmp", "image/ms-bmp", "image/x-ms-bmp",
        "application/bmp", "application/x-bmp", "application/x-win-bitmap",
    };
    for (auto& type : malformedMIMETypes) {
        supportedImageMIMETypes->add(type);
        supportedImageResourceMIMETypes->add(type);
    }
#endif

#else
    // assume that all implementations at least support the following standard
    // image types:
    static const char* types[] = {
        "image/jpeg",
        "image/png",
        "image/gif",
        "image/bmp",
        "image/vnd.microsoft.icon",    // ico
        "image/x-icon",    // ico
        "image/x-xbitmap"  // xbm
    };
    for (auto& type : types) {
        supportedImageMIMETypes->add(type);
        supportedImageResourceMIMETypes->add(type);
    }

#if USE(WEBP)
    supportedImageMIMETypes->add("image/webp");
    supportedImageResourceMIMETypes->add("image/webp");
#endif

#if PLATFORM(QT)
    const QList<QByteArray> mimeTypes = QImageReader::supportedMimeTypes();
    for (const QByteArray& mimeType : mimeTypes) {
        if (mimeType.isEmpty())
            continue;
        supportedImageMIMETypes->add(mimeType.constData());
        supportedImageResourceMIMETypes->add(mimeType.constData());
    }
    // Do not treat SVG as images directly because WebKit can handle them.
    supportedImageMIMETypes->remove("image/svg+xml");
    supportedImageResourceMIMETypes->remove("image/svg+xml");
    // Do not treat PDF as images
    supportedImageMIMETypes->remove("application/pdf");
    supportedImageResourceMIMETypes->remove("application/pdf");
#endif // PLATFORM(QT)
#endif // USE(CG)
}

static void initializeSupportedImageMIMETypesForEncoding()
{
    supportedImageMIMETypesForEncoding = new HashSet<String, ASCIICaseInsensitiveHash>;

#if USE(CG)
#if PLATFORM(COCOA)
    RetainPtr<CFArrayRef> supportedTypes = adoptCF(CGImageDestinationCopyTypeIdentifiers());
    CFIndex count = CFArrayGetCount(supportedTypes.get());
    for (CFIndex i = 0; i < count; i++) {
        CFStringRef supportedType = reinterpret_cast<CFStringRef>(CFArrayGetValueAtIndex(supportedTypes.get(), i));
        String mimeType = MIMETypeForImageSourceType(supportedType);
        if (!mimeType.isEmpty())
            supportedImageMIMETypesForEncoding->add(mimeType);
    }
#else
    // FIXME: Add Windows support for all the supported UTI's when a way to convert from MIMEType to UTI reliably is found.
    // For now, only support PNG, JPEG and GIF.  See <rdar://problem/6095286>.
    supportedImageMIMETypesForEncoding->add("image/png");
    supportedImageMIMETypesForEncoding->add("image/jpeg");
    supportedImageMIMETypesForEncoding->add("image/gif");
#endif
#elif PLATFORM(QT)
    const QList<QByteArray> mimeTypes = QImageWriter::supportedMimeTypes();
    for (const QByteArray& mimeType : mimeTypes) {
        if (mimeType.isEmpty())
            continue;
        supportedImageMIMETypesForEncoding->add(mimeType.constData());
    }
#elif PLATFORM(GTK)
    supportedImageMIMETypesForEncoding->add("image/png");
    supportedImageMIMETypesForEncoding->add("image/jpeg");
    supportedImageMIMETypesForEncoding->add("image/tiff");
    supportedImageMIMETypesForEncoding->add("image/bmp");
    supportedImageMIMETypesForEncoding->add("image/ico");
#elif PLATFORM(EFL)
    supportedImageMIMETypesForEncoding->add("image/png");
    supportedImageMIMETypesForEncoding->add("image/jpeg");
#elif USE(CAIRO)
    supportedImageMIMETypesForEncoding->add("image/png");
#endif
}

static void initializeSupportedJavaScriptMIMETypes()
{
    // https://html.spec.whatwg.org/multipage/scripting.html#javascript-mime-type
    static const char* types[] = {
        "text/javascript",
        "text/ecmascript",
        "application/javascript",
        "application/ecmascript",
        "application/x-javascript",
        "application/x-ecmascript",
        "text/javascript1.0",
        "text/javascript1.1",
        "text/javascript1.2",
        "text/javascript1.3",
        "text/javascript1.4",
        "text/javascript1.5",
        "text/jscript",
        "text/livescript",
        "text/x-javascript",
        "text/x-ecmascript"
    };
    for (auto* type : types)
        supportedJavaScriptMIMETypes->add(type);
}

static void initializePDFMIMETypes()
{
    const char* const types[] = {
        "application/pdf",
        "text/pdf"
    };
    for (auto& type : types)
        pdfMIMETypes->add(type);
}

static void initializePostScriptMIMETypes()
{
    pdfAndPostScriptMIMETypes->add("application/postscript");
}

static void initializeSupportedNonImageMimeTypes()
{
    static const char* types[] = {
        "text/html",
        "text/xml",
        "text/xsl",
        "text/plain",
        "text/",
        "application/xml",
        "application/xhtml+xml",
#if !PLATFORM(IOS)
        "application/vnd.wap.xhtml+xml",
        "application/rss+xml",
        "application/atom+xml",
#endif
        "application/json",
        "image/svg+xml",
#if ENABLE(FTPDIR)
        "application/x-ftp-directory",
#endif
        "multipart/x-mixed-replace"
        // Note: Adding a new type here will probably render it as HTML.
        // This can result in cross-site scripting vulnerabilities.
    };

    for (auto& type : types)
        supportedNonImageMIMETypes->add(type);

#if ENABLE(WEB_ARCHIVE) || ENABLE(MHTML)
    ArchiveFactory::registerKnownArchiveMIMETypes();
#endif
}

static MediaMIMETypeMap& mediaMIMETypeMap()
{
    static NeverDestroyed<MediaMIMETypeMap> mediaMIMETypeForExtensionMap;

    if (!mediaMIMETypeForExtensionMap.get().isEmpty())
        return mediaMIMETypeForExtensionMap;

    const unsigned numPairs = sizeof(commonMediaTypes) / sizeof(commonMediaTypes[0]);
    for (unsigned ndx = 0; ndx < numPairs; ++ndx) {

        if (mediaMIMETypeForExtensionMap.get().contains(commonMediaTypes[ndx].extension))
            mediaMIMETypeForExtensionMap.get().get(commonMediaTypes[ndx].extension)->append(commonMediaTypes[ndx].type);
        else {
            Vector<String>* synonyms = new Vector<String>;

            // If there is a system specific type for this extension, add it as the first type so
            // getMediaMIMETypeForExtension will always return it.
            String systemType = MIMETypeRegistry::getMIMETypeForExtension(commonMediaTypes[ndx].extension);
            if (!systemType.isEmpty() && commonMediaTypes[ndx].type != systemType)
                synonyms->append(systemType);
            synonyms->append(commonMediaTypes[ndx].type);
            mediaMIMETypeForExtensionMap.get().add(commonMediaTypes[ndx].extension, synonyms);
        }
    }

    return mediaMIMETypeForExtensionMap;
}

String MIMETypeRegistry::getMediaMIMETypeForExtension(const String& ext)
{
    // Look in the system-specific registry first.
    String type = getMIMETypeForExtension(ext);
    if (!type.isEmpty())
        return type;

    Vector<String>* typeList = mediaMIMETypeMap().get(ext);
    if (typeList)
        return (*typeList)[0];
    
    return String();
}
    
Vector<String> MIMETypeRegistry::getMediaMIMETypesForExtension(const String& ext)
{
    Vector<String>* typeList = mediaMIMETypeMap().get(ext);
    if (typeList)
        return *typeList;

    // Only need to look in the system-specific registry if mediaMIMETypeMap() doesn't contain
    // the extension at all, because it always contains the system-specific type if the
    // extension is in the static mapping table.
    String type = getMIMETypeForExtension(ext);
    if (!type.isEmpty()) {
        Vector<String> typeList;
        typeList.append(type);
        return typeList;
    }
    
    return Vector<String>();
}

static void initializeSupportedMediaMIMETypes()
{
    supportedMediaMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>;
#if ENABLE(VIDEO)
    MediaPlayer::getSupportedTypes(*supportedMediaMIMETypes);
#endif
}

static void initializeUnsupportedTextMIMETypes()
{
    static const char* types[] = {
        "text/calendar",
        "text/x-calendar",
        "text/x-vcalendar",
        "text/vcalendar",
        "text/vcard",
        "text/x-vcard",
        "text/directory",
        "text/ldif",
        "text/qif",
        "text/x-qif",
        "text/x-csv",
        "text/x-vcf",
#if !PLATFORM(IOS)
        "text/rtf",
#else
        "text/vnd.sun.j2me.app-descriptor",
#endif
    };
    for (auto& type : types)
        unsupportedTextMIMETypes->add(type);
}

static void initializeMIMETypeRegistry()
{
    supportedJavaScriptMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>;
    initializeSupportedJavaScriptMIMETypes();

    supportedNonImageMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>(*supportedJavaScriptMIMETypes);
    initializeSupportedNonImageMimeTypes();

    supportedImageResourceMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>;
    supportedImageMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>;
    initializeSupportedImageMIMETypes();

    pdfMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>;
    initializePDFMIMETypes();

    pdfAndPostScriptMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>(*pdfMIMETypes);
    initializePostScriptMIMETypes();

    unsupportedTextMIMETypes = new HashSet<String, ASCIICaseInsensitiveHash>;
    initializeUnsupportedTextMIMETypes();
}

#if !PLATFORM(QT)
String MIMETypeRegistry::getMIMETypeForPath(const String& path)
{
    size_t pos = path.reverseFind('.');
    if (pos != notFound) {
        String extension = path.substring(pos + 1);
        String result = getMIMETypeForExtension(extension);
        if (result.length())
            return result;
    }
    return defaultMIMEType();
}
#endif

bool MIMETypeRegistry::isSupportedImageMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!supportedImageMIMETypes)
        initializeMIMETypeRegistry();
    return supportedImageMIMETypes->contains(getNormalizedMIMEType(mimeType));
}

bool MIMETypeRegistry::isSupportedImageResourceMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!supportedImageResourceMIMETypes)
        initializeMIMETypeRegistry();
    return supportedImageResourceMIMETypes->contains(getNormalizedMIMEType(mimeType));
}

bool MIMETypeRegistry::isSupportedImageMIMETypeForEncoding(const String& mimeType)
{
    ASSERT(isMainThread());

    if (mimeType.isEmpty())
        return false;
    if (!supportedImageMIMETypesForEncoding)
        initializeSupportedImageMIMETypesForEncoding();
    return supportedImageMIMETypesForEncoding->contains(mimeType);
}

bool MIMETypeRegistry::isSupportedJavaScriptMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!supportedJavaScriptMIMETypes)
        initializeMIMETypeRegistry();
    return supportedJavaScriptMIMETypes->contains(mimeType);
}

bool MIMETypeRegistry::isSupportedNonImageMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!supportedNonImageMIMETypes)
        initializeMIMETypeRegistry();
    return supportedNonImageMIMETypes->contains(mimeType);
}

bool MIMETypeRegistry::isSupportedMediaMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!supportedMediaMIMETypes)
        initializeSupportedMediaMIMETypes();
    return supportedMediaMIMETypes->contains(mimeType);
}

bool MIMETypeRegistry::isUnsupportedTextMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!unsupportedTextMIMETypes)
        initializeMIMETypeRegistry();
    return unsupportedTextMIMETypes->contains(mimeType);
}

bool MIMETypeRegistry::isJavaAppletMIMEType(const String& mimeType)
{
    // Since this set is very limited and is likely to remain so we won't bother with the overhead
    // of using a hash set.
    // Any of the MIME types below may be followed by any number of specific versions of the JVM,
    // which is why we use startsWith()
    return mimeType.startsWith("application/x-java-applet", false)
        || mimeType.startsWith("application/x-java-bean", false)
        || mimeType.startsWith("application/x-java-vm", false);
}

bool MIMETypeRegistry::isPDFOrPostScriptMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!pdfAndPostScriptMIMETypes)
        initializeMIMETypeRegistry();
    return pdfAndPostScriptMIMETypes->contains(mimeType);
}

bool MIMETypeRegistry::isPDFMIMEType(const String& mimeType)
{
    if (mimeType.isEmpty())
        return false;
    if (!pdfMIMETypes)
        initializeMIMETypeRegistry();
    return pdfMIMETypes->contains(mimeType);
}

bool MIMETypeRegistry::canShowMIMEType(const String& mimeType)
{
    if (isSupportedImageMIMEType(mimeType) || isSupportedNonImageMIMEType(mimeType) || isSupportedMediaMIMEType(mimeType))
        return true;

    if (mimeType.startsWith("text/", false))
        return !MIMETypeRegistry::isUnsupportedTextMIMEType(mimeType);

    return false;
}

HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getSupportedImageMIMETypes()
{
    if (!supportedImageMIMETypes)
        initializeMIMETypeRegistry();
    return *supportedImageMIMETypes;
}

HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getSupportedImageResourceMIMETypes()
{
    if (!supportedImageResourceMIMETypes)
        initializeMIMETypeRegistry();
    return *supportedImageResourceMIMETypes;
}

HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getSupportedImageMIMETypesForEncoding()
{
    if (!supportedImageMIMETypesForEncoding)
        initializeSupportedImageMIMETypesForEncoding();
    return *supportedImageMIMETypesForEncoding;
}

HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getSupportedNonImageMIMETypes()
{
    if (!supportedNonImageMIMETypes)
        initializeMIMETypeRegistry();
    return *supportedNonImageMIMETypes;
}

HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getSupportedMediaMIMETypes()
{
    if (!supportedMediaMIMETypes)
        initializeSupportedMediaMIMETypes();
    return *supportedMediaMIMETypes;
}


HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getPDFMIMETypes()
{
    if (!pdfMIMETypes)
        initializeMIMETypeRegistry();
    return *pdfMIMETypes;
}

HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getPDFAndPostScriptMIMETypes()
{
    if (!pdfAndPostScriptMIMETypes)
        initializeMIMETypeRegistry();
    return *pdfAndPostScriptMIMETypes;
}

HashSet<String, ASCIICaseInsensitiveHash>& MIMETypeRegistry::getUnsupportedTextMIMETypes()
{
    if (!unsupportedTextMIMETypes)
        initializeMIMETypeRegistry();
    return *unsupportedTextMIMETypes;
}

const String& defaultMIMEType()
{
    static NeverDestroyed<const String> defaultMIMEType(ASCIILiteral("application/octet-stream"));
    return defaultMIMEType;
}

#if !PLATFORM(QT)

#if !USE(CURL)

// FIXME: Not sure why it makes sense to have a cross-platform function when only CURL has the concept
// of a "normalized" MIME type.
String MIMETypeRegistry::getNormalizedMIMEType(const String& mimeType)
{
    return mimeType;
}

#else

typedef HashMap<String, String, ASCIICaseInsensitiveHash> MIMETypeAssociationMap;

static const MIMETypeAssociationMap& mimeTypeAssociationMap()
{
    static MIMETypeAssociationMap* mimeTypeMap = 0;
    if (mimeTypeMap)
        return *mimeTypeMap;

    // FIXME: Should not allocate this on the heap; use NeverDestroyed instead.
    mimeTypeMap = new MIMETypeAssociationMap;

    // FIXME: Writing the function out like this will create a giant function.
    // Should use a loop instead.
    mimeTypeMap->add(ASCIILiteral("image/x-ms-bmp"), ASCIILiteral("image/bmp"));
    mimeTypeMap->add(ASCIILiteral("image/x-windows-bmp"), ASCIILiteral("image/bmp"));
    mimeTypeMap->add(ASCIILiteral("image/x-bmp"), ASCIILiteral("image/bmp"));
    mimeTypeMap->add(ASCIILiteral("image/x-bitmap"), ASCIILiteral("image/bmp"));
    mimeTypeMap->add(ASCIILiteral("image/x-ms-bitmap"), ASCIILiteral("image/bmp"));
    mimeTypeMap->add(ASCIILiteral("image/jpg"), ASCIILiteral("image/jpeg"));
    mimeTypeMap->add(ASCIILiteral("image/pjpeg"), ASCIILiteral("image/jpeg"));
    mimeTypeMap->add(ASCIILiteral("image/x-png"), ASCIILiteral("image/png"));
    mimeTypeMap->add(ASCIILiteral("image/vnd.rim.png"), ASCIILiteral("image/png"));
    mimeTypeMap->add(ASCIILiteral("image/ico"), ASCIILiteral("image/vnd.microsoft.icon"));
    mimeTypeMap->add(ASCIILiteral("image/icon"), ASCIILiteral("image/vnd.microsoft.icon"));
    mimeTypeMap->add(ASCIILiteral("text/ico"), ASCIILiteral("image/vnd.microsoft.icon"));
    mimeTypeMap->add(ASCIILiteral("application/ico"), ASCIILiteral("image/vnd.microsoft.icon"));
    mimeTypeMap->add(ASCIILiteral("image/x-icon"), ASCIILiteral("image/vnd.microsoft.icon"));
    mimeTypeMap->add(ASCIILiteral("audio/vnd.qcelp"), ASCIILiteral("audio/qcelp"));
    mimeTypeMap->add(ASCIILiteral("audio/qcp"), ASCIILiteral("audio/qcelp"));
    mimeTypeMap->add(ASCIILiteral("audio/vnd.qcp"), ASCIILiteral("audio/qcelp"));
    mimeTypeMap->add(ASCIILiteral("audio/wav"), ASCIILiteral("audio/x-wav"));
    mimeTypeMap->add(ASCIILiteral("audio/mid"), ASCIILiteral("audio/midi"));
    mimeTypeMap->add(ASCIILiteral("audio/sp-midi"), ASCIILiteral("audio/midi"));
    mimeTypeMap->add(ASCIILiteral("audio/x-mid"), ASCIILiteral("audio/midi"));
    mimeTypeMap->add(ASCIILiteral("audio/x-midi"), ASCIILiteral("audio/midi"));
    mimeTypeMap->add(ASCIILiteral("audio/x-mpeg"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/mp3"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/x-mp3"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/mpeg3"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/x-mpeg3"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/mpg3"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/mpg"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/x-mpg"), ASCIILiteral("audio/mpeg"));
    mimeTypeMap->add(ASCIILiteral("audio/m4a"), ASCIILiteral("audio/mp4"));
    mimeTypeMap->add(ASCIILiteral("audio/x-m4a"), ASCIILiteral("audio/mp4"));
    mimeTypeMap->add(ASCIILiteral("audio/x-mp4"), ASCIILiteral("audio/mp4"));
    mimeTypeMap->add(ASCIILiteral("audio/x-aac"), ASCIILiteral("audio/aac"));
    mimeTypeMap->add(ASCIILiteral("audio/x-amr"), ASCIILiteral("audio/amr"));
    mimeTypeMap->add(ASCIILiteral("audio/mpegurl"), ASCIILiteral("audio/x-mpegurl"));
    mimeTypeMap->add(ASCIILiteral("audio/flac"), ASCIILiteral("audio/x-flac"));
    mimeTypeMap->add(ASCIILiteral("video/3gp"), ASCIILiteral("video/3gpp"));
    mimeTypeMap->add(ASCIILiteral("video/avi"), ASCIILiteral("video/x-msvideo"));
    mimeTypeMap->add(ASCIILiteral("video/x-m4v"), ASCIILiteral("video/mp4"));
    mimeTypeMap->add(ASCIILiteral("video/x-quicktime"), ASCIILiteral("video/quicktime"));
    mimeTypeMap->add(ASCIILiteral("application/java"), ASCIILiteral("application/java-archive"));
    mimeTypeMap->add(ASCIILiteral("application/x-java-archive"), ASCIILiteral("application/java-archive"));
    mimeTypeMap->add(ASCIILiteral("application/x-zip-compressed"), ASCIILiteral("application/zip"));
    mimeTypeMap->add(ASCIILiteral("text/cache-manifest"), ASCIILiteral("text/plain"));

    return *mimeTypeMap;
}

String MIMETypeRegistry::getNormalizedMIMEType(const String& mimeType)
{
    auto it = mimeTypeAssociationMap().find(mimeType);
    if (it != mimeTypeAssociationMap().end())
        return it->value;
    return mimeType;
}

#endif

#endif // !PLATFORM(QT)

} // namespace WebCore
