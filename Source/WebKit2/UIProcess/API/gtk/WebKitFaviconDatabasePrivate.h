/*
 * Copyright (C) 2012 Igalia S.L.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef WebKitFaviconDatabasePrivate_h
#define WebKitFaviconDatabasePrivate_h

#include "WebIconDatabase.h"
#include "WebKitFaviconDatabase.h"
#include <wtf/text/CString.h>

using namespace WebKit;

WebKitFaviconDatabase* webkitFaviconDatabaseCreate(WebIconDatabase*);
cairo_surface_t* webkitFaviconDatabaseGetFavicon(WebKitFaviconDatabase*, const CString&);

#endif // WebKitFaviconDatabasePrivate_h