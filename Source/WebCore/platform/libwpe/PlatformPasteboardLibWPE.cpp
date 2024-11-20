/*
 * Copyright (C) 2015 Igalia S.L.
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
#include "PlatformPasteboard.h"

#if USE(LIBWPE)

#include "Pasteboard.h"
#include <wpe/wpe.h>
#include <wtf/Assertions.h>
#include <wtf/HashMap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

static UncheckedKeyHashMap<String, String>& sharedPasteboard()
{
    static NeverDestroyed<UncheckedKeyHashMap<String, String>> pasteboard;
    return pasteboard.get();
}

PlatformPasteboard::PlatformPasteboard(const String&)
    : m_pasteboard(wpe_pasteboard_get_singleton())
{
    ASSERT(m_pasteboard);
}

PlatformPasteboard::PlatformPasteboard()
    : m_pasteboard(wpe_pasteboard_get_singleton())
{
    ASSERT(m_pasteboard);
}

void PlatformPasteboard::performAsDataOwner(DataOwnerType, Function<void()>&& actions)
{
    actions();
}

void PlatformPasteboard::getTypes(Vector<String>& types) const
{
<<<<<<< HEAD
    struct wpe_pasteboard_string_vector pasteboardTypes = { nullptr, 0 };
    wpe_pasteboard_get_types(m_pasteboard, &pasteboardTypes);

    for (unsigned i = 0; i < pasteboardTypes.length; ++i) {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN // WPE port
        auto& typeString = pasteboardTypes.strings[i];
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
        const auto length = std::min(static_cast<size_t>(typeString.length), std::numeric_limits<size_t>::max());
        types.append(String({ typeString.data, length }));
    }

    wpe_pasteboard_string_vector_free(&pasteboardTypes);
||||||| parent of f69f958318e1 (chore(webkit): bootstrap build #2106)
    struct wpe_pasteboard_string_vector pasteboardTypes = { nullptr, 0 };
    wpe_pasteboard_get_types(m_pasteboard, &pasteboardTypes);

    for (unsigned i = 0; i < pasteboardTypes.length; ++i) {
        auto& typeString = pasteboardTypes.strings[i];
        const auto length = std::min(static_cast<size_t>(typeString.length), std::numeric_limits<size_t>::max());
        types.append(String({ typeString.data, length }));
    }

    wpe_pasteboard_string_vector_free(&pasteboardTypes);
=======
    for (const auto& type : sharedPasteboard().keys())
        types.append(type);
>>>>>>> f69f958318e1 (chore(webkit): bootstrap build #2106)
}

String PlatformPasteboard::readString(size_t, const String& type) const
{
    return sharedPasteboard().get(type);
}

void PlatformPasteboard::write(const PasteboardWebContent& content)
{
<<<<<<< HEAD
    static const char plainText[] = "text/plain;charset=utf-8";
    static const char htmlText[] = "text/html;charset=utf-8";

    CString textString = content.text.utf8();
    CString markupString = content.markup.utf8();

    WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN // WPE port
    struct wpe_pasteboard_string_pair pairs[] = {
        { { nullptr, 0 }, { nullptr, 0 } },
        { { nullptr, 0 }, { nullptr, 0 } },
    };
    WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    wpe_pasteboard_string_initialize(&pairs[0].type, plainText, strlen(plainText));
    wpe_pasteboard_string_initialize(&pairs[0].string, textString.data(), textString.length());
    wpe_pasteboard_string_initialize(&pairs[1].type, htmlText, strlen(htmlText));
    wpe_pasteboard_string_initialize(&pairs[1].string, markupString.data(), markupString.length());
    struct wpe_pasteboard_string_map map = { pairs, 2 };

    wpe_pasteboard_write(m_pasteboard, &map);

    wpe_pasteboard_string_free(&pairs[0].type);
    wpe_pasteboard_string_free(&pairs[0].string);
    wpe_pasteboard_string_free(&pairs[1].type);
    wpe_pasteboard_string_free(&pairs[1].string);
||||||| parent of f69f958318e1 (chore(webkit): bootstrap build #2106)
    static const char plainText[] = "text/plain;charset=utf-8";
    static const char htmlText[] = "text/html;charset=utf-8";

    CString textString = content.text.utf8();
    CString markupString = content.markup.utf8();

    struct wpe_pasteboard_string_pair pairs[] = {
        { { nullptr, 0 }, { nullptr, 0 } },
        { { nullptr, 0 }, { nullptr, 0 } },
    };
    wpe_pasteboard_string_initialize(&pairs[0].type, plainText, strlen(plainText));
    wpe_pasteboard_string_initialize(&pairs[0].string, textString.data(), textString.length());
    wpe_pasteboard_string_initialize(&pairs[1].type, htmlText, strlen(htmlText));
    wpe_pasteboard_string_initialize(&pairs[1].string, markupString.data(), markupString.length());
    struct wpe_pasteboard_string_map map = { pairs, 2 };

    wpe_pasteboard_write(m_pasteboard, &map);

    wpe_pasteboard_string_free(&pairs[0].type);
    wpe_pasteboard_string_free(&pairs[0].string);
    wpe_pasteboard_string_free(&pairs[1].type);
    wpe_pasteboard_string_free(&pairs[1].string);
=======
    String plainText = "text/plain;charset=utf-8"_s;
    String htmlText = "text/html;charset=utf-8"_s;
    sharedPasteboard().set(plainText, content.text);
    sharedPasteboard().set(htmlText, content.markup);
>>>>>>> f69f958318e1 (chore(webkit): bootstrap build #2106)
}

void PlatformPasteboard::write(const String& type, const String& string)
{
    sharedPasteboard().set(type, string);
}

Vector<String> PlatformPasteboard::typesSafeForDOMToReadAndWrite(const String&) const
{
    return { };
}

int64_t PlatformPasteboard::write(const PasteboardCustomData&)
{
    return 0;
}

int64_t PlatformPasteboard::write(const Vector<PasteboardCustomData>&)
{
    return 0;
}

} // namespace WebCore

#endif // USE(LIBWPE)
