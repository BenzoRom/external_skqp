/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkFontArguments.h"
#include "SkFontMgr.h"
#include "SkLoadICU.h"
#include "SkMalloc.h"
#include "SkOnce.h"
#include "SkFont.h"
#include "SkFontMetrics.h"
#include "SkPoint.h"
#include "SkRefCnt.h"
#include "SkScalar.h"
#include "SkShaper.h"
#include "SkStream.h"
#include "SkString.h"
#include "SkTArray.h"
#include "SkTDPQueue.h"
#include "SkTFitsIn.h"
#include "SkTLazy.h"
#include "SkTemplates.h"
#include "SkTo.h"
#include "SkTypeface.h"
#include "SkTypes.h"
#include "SkUTF.h"

#include <hb.h>
#include <hb-ot.h>
#include <unicode/ubrk.h>
#include <unicode/ubidi.h>
#include <unicode/ustring.h>
#include <unicode/urename.h>
#include <unicode/utext.h>
#include <unicode/utypes.h>

#include <memory>
#include <utility>
#include <cstring>

namespace {
template <class T, void(*P)(T*)> using resource = std::unique_ptr<T, SkFunctionWrapper<void, T, P>>;
using HBBlob   = resource<hb_blob_t     , hb_blob_destroy  >;
using HBFace   = resource<hb_face_t     , hb_face_destroy  >;
using HBFont   = resource<hb_font_t     , hb_font_destroy  >;
using HBBuffer = resource<hb_buffer_t   , hb_buffer_destroy>;
using ICUBiDi  = resource<UBiDi         , ubidi_close      >;
using ICUBrk   = resource<UBreakIterator, ubrk_close      >;

HBBlob stream_to_blob(std::unique_ptr<SkStreamAsset> asset) {
    size_t size = asset->getLength();
    HBBlob blob;
    if (const void* base = asset->getMemoryBase()) {
        blob.reset(hb_blob_create((char*)base, SkToUInt(size),
                                  HB_MEMORY_MODE_READONLY, asset.release(),
                                  [](void* p) { delete (SkStreamAsset*)p; }));
    } else {
        // SkDebugf("Extra SkStreamAsset copy\n");
        void* ptr = size ? sk_malloc_throw(size) : nullptr;
        asset->read(ptr, size);
        blob.reset(hb_blob_create((char*)ptr, SkToUInt(size),
                                  HB_MEMORY_MODE_READONLY, ptr, sk_free));
    }
    SkASSERT(blob);
    hb_blob_make_immutable(blob.get());
    return blob;
}

HBFont create_hb_font(SkTypeface* tf) {
    if (!tf) {
        return nullptr;
    }
    int index;
    std::unique_ptr<SkStreamAsset> typefaceAsset(tf->openStream(&index));
    if (!typefaceAsset) {
        SkString name;
        tf->getFamilyName(&name);
        SkDebugf("Typeface '%s' has no data :(\n", name.c_str());
        return nullptr;
    }
    HBBlob blob(stream_to_blob(std::move(typefaceAsset)));
    HBFace face(hb_face_create(blob.get(), (unsigned)index));
    SkASSERT(face);
    if (!face) {
        return nullptr;
    }
    hb_face_set_index(face.get(), (unsigned)index);
    hb_face_set_upem(face.get(), tf->getUnitsPerEm());

    HBFont font(hb_font_create(face.get()));
    SkASSERT(font);
    if (!font) {
        return nullptr;
    }
    hb_ot_font_set_funcs(font.get());
    int axis_count = tf->getVariationDesignPosition(nullptr, 0);
    if (axis_count > 0) {
        SkAutoSTMalloc<4, SkFontArguments::VariationPosition::Coordinate> axis_values(axis_count);
        if (tf->getVariationDesignPosition(axis_values, axis_count) == axis_count) {
            hb_font_set_variations(font.get(),
                                   reinterpret_cast<hb_variation_t*>(axis_values.get()),
                                   axis_count);
        }
    }
    return font;
}

/** this version replaces invalid utf-8 sequences with code point U+FFFD. */
static inline SkUnichar utf8_next(const char** ptr, const char* end) {
    SkUnichar val = SkUTF::NextUTF8(ptr, end);
    if (val < 0) {
        return 0xFFFD;  // REPLACEMENT CHARACTER
    }
    return val;
}

class RunIterator {
public:
    virtual ~RunIterator() {}
    virtual void consume() = 0;
    // Pointer one past the last (utf8) element in the current run.
    virtual const char* endOfCurrentRun() const = 0;
    virtual bool atEnd() const = 0;
    bool operator<(const RunIterator& that) const {
        return this->endOfCurrentRun() < that.endOfCurrentRun();
    }
};

class BiDiRunIterator : public RunIterator {
public:
    static SkTLazy<BiDiRunIterator> Make(const char* utf8, size_t utf8Bytes, UBiDiLevel level) {
        SkTLazy<BiDiRunIterator> ret;

        // ubidi only accepts utf16 (though internally it basically works on utf32 chars).
        // We want an ubidi_setPara(UBiDi*, UText*, UBiDiLevel, UBiDiLevel*, UErrorCode*);
        if (!SkTFitsIn<int32_t>(utf8Bytes)) {
            SkDebugf("Bidi error: text too long");
            return ret;
        }

        UErrorCode status = U_ZERO_ERROR;

        // Getting the length like this seems to always set U_BUFFER_OVERFLOW_ERROR
        int32_t utf16Units;
        u_strFromUTF8(nullptr, 0, &utf16Units, utf8, utf8Bytes, &status);
        status = U_ZERO_ERROR;
        std::unique_ptr<UChar[]> utf16(new UChar[utf16Units]);
        u_strFromUTF8(utf16.get(), utf16Units, nullptr, utf8, utf8Bytes, &status);
        if (U_FAILURE(status)) {
            SkDebugf("Invalid utf8 input: %s", u_errorName(status));
            return ret;
        }

        ICUBiDi bidi(ubidi_openSized(utf16Units, 0, &status));
        if (U_FAILURE(status)) {
            SkDebugf("Bidi error: %s", u_errorName(status));
            return ret;
        }
        SkASSERT(bidi);

        // The required lifetime of utf16 isn't well documented.
        // It appears it isn't used after ubidi_setPara except through ubidi_getText.
        ubidi_setPara(bidi.get(), utf16.get(), utf16Units, level, nullptr, &status);
        if (U_FAILURE(status)) {
            SkDebugf("Bidi error: %s", u_errorName(status));
            return ret;
        }

        ret.init(utf8, utf8 + utf8Bytes, std::move(bidi));
        return ret;
    }
    BiDiRunIterator(const char* utf8, const char* end, ICUBiDi bidi)
        : fBidi(std::move(bidi))
        , fEndOfCurrentRun(utf8)
        , fEndOfAllRuns(end)
        , fUTF16LogicalPosition(0)
        , fLevel(UBIDI_DEFAULT_LTR)
    {}
    void consume() override {
        SkASSERT(fUTF16LogicalPosition < ubidi_getLength(fBidi.get()));
        int32_t endPosition = ubidi_getLength(fBidi.get());
        fLevel = ubidi_getLevelAt(fBidi.get(), fUTF16LogicalPosition);
        SkUnichar u = utf8_next(&fEndOfCurrentRun, fEndOfAllRuns);
        fUTF16LogicalPosition += SkUTF::ToUTF16(u);
        UBiDiLevel level;
        while (fUTF16LogicalPosition < endPosition) {
            level = ubidi_getLevelAt(fBidi.get(), fUTF16LogicalPosition);
            if (level != fLevel) {
                break;
            }
            u = utf8_next(&fEndOfCurrentRun, fEndOfAllRuns);
            fUTF16LogicalPosition += SkUTF::ToUTF16(u);
        }
    }
    const char* endOfCurrentRun() const override {
        return fEndOfCurrentRun;
    }
    bool atEnd() const override {
        return fUTF16LogicalPosition == ubidi_getLength(fBidi.get());
    }

    UBiDiLevel currentLevel() const {
        return fLevel;
    }
private:
    ICUBiDi fBidi;
    const char* fEndOfCurrentRun;
    const char* fEndOfAllRuns;
    int32_t fUTF16LogicalPosition;
    UBiDiLevel fLevel;
};

class ScriptRunIterator : public RunIterator {
public:
    static SkTLazy<ScriptRunIterator> Make(const char* utf8, size_t utf8Bytes,
                                           hb_unicode_funcs_t* hbUnicode)
    {
        SkTLazy<ScriptRunIterator> ret;
        ret.init(utf8, utf8Bytes, hbUnicode);
        return ret;
    }
    ScriptRunIterator(const char* utf8, size_t utf8Bytes, hb_unicode_funcs_t* hbUnicode)
        : fCurrent(utf8), fEnd(fCurrent + utf8Bytes)
        , fHBUnicode(hbUnicode)
        , fCurrentScript(HB_SCRIPT_UNKNOWN)
    {}
    void consume() override {
        SkASSERT(fCurrent < fEnd);
        SkUnichar u = utf8_next(&fCurrent, fEnd);
        fCurrentScript = hb_unicode_script(fHBUnicode, u);
        while (fCurrent < fEnd) {
            const char* prev = fCurrent;
            u = utf8_next(&fCurrent, fEnd);
            const hb_script_t script = hb_unicode_script(fHBUnicode, u);
            if (script != fCurrentScript) {
                if (fCurrentScript == HB_SCRIPT_INHERITED || fCurrentScript == HB_SCRIPT_COMMON) {
                    fCurrentScript = script;
                } else if (script == HB_SCRIPT_INHERITED || script == HB_SCRIPT_COMMON) {
                    continue;
                } else {
                    fCurrent = prev;
                    break;
                }
            }
        }
        if (fCurrentScript == HB_SCRIPT_INHERITED) {
            fCurrentScript = HB_SCRIPT_COMMON;
        }
    }
    const char* endOfCurrentRun() const override {
        return fCurrent;
    }
    bool atEnd() const override {
        return fCurrent == fEnd;
    }

    hb_script_t currentScript() const {
        return fCurrentScript;
    }
private:
    const char* fCurrent;
    const char* fEnd;
    hb_unicode_funcs_t* fHBUnicode;
    hb_script_t fCurrentScript;
};

class FontRunIterator : public RunIterator {
public:
    static SkTLazy<FontRunIterator> Make(const char* utf8, size_t utf8Bytes,
                                         sk_sp<SkTypeface> typeface,
                                         hb_font_t* hbFace,
                                         sk_sp<SkFontMgr> fallbackMgr)
    {
        SkTLazy<FontRunIterator> ret;
        ret.init(utf8, utf8Bytes, std::move(typeface), hbFace, std::move(fallbackMgr));
        return ret;
    }
    FontRunIterator(const char* utf8, size_t utf8Bytes, sk_sp<SkTypeface> typeface,
                    hb_font_t* hbFace, sk_sp<SkFontMgr> fallbackMgr)
        : fCurrent(utf8), fEnd(fCurrent + utf8Bytes)
        , fFallbackMgr(std::move(fallbackMgr))
        , fHBFont(hbFace), fTypeface(std::move(typeface))
        , fFallbackHBFont(nullptr), fFallbackTypeface(nullptr)
        , fCurrentHBFont(fHBFont), fCurrentTypeface(fTypeface.get())
    {}
    void consume() override {
        SkASSERT(fCurrent < fEnd);
        SkUnichar u = utf8_next(&fCurrent, fEnd);
        // If the starting typeface can handle this character, use it.
        if (fTypeface->charsToGlyphs(&u, SkTypeface::kUTF32_Encoding, nullptr, 1)) {
            fCurrentTypeface = fTypeface.get();
            fCurrentHBFont = fHBFont;
        // If the current fallback can handle this character, use it.
        } else if (fFallbackTypeface &&
                   fFallbackTypeface->charsToGlyphs(&u, SkTypeface::kUTF32_Encoding, nullptr, 1))
        {
            fCurrentTypeface = fFallbackTypeface.get();
            fCurrentHBFont = fFallbackHBFont.get();
        // If not, try to find a fallback typeface
        } else {
            fFallbackTypeface.reset(fFallbackMgr->matchFamilyStyleCharacter(
                nullptr, fTypeface->fontStyle(), nullptr, 0, u));
            fFallbackHBFont = create_hb_font(fFallbackTypeface.get());
            fCurrentTypeface = fFallbackTypeface.get();
            fCurrentHBFont = fFallbackHBFont.get();
        }

        while (fCurrent < fEnd) {
            const char* prev = fCurrent;
            u = utf8_next(&fCurrent, fEnd);

            // If not using initial typeface and initial typeface has this character, stop fallback.
            if (fCurrentTypeface != fTypeface.get() &&
                fTypeface->charsToGlyphs(&u, SkTypeface::kUTF32_Encoding, nullptr, 1))
            {
                fCurrent = prev;
                return;
            }
            // If the current typeface cannot handle this character, stop using it.
            if (!fCurrentTypeface->charsToGlyphs(&u, SkTypeface::kUTF32_Encoding, nullptr, 1)) {
                fCurrent = prev;
                return;
            }
        }
    }
    const char* endOfCurrentRun() const override {
        return fCurrent;
    }
    bool atEnd() const override {
        return fCurrent == fEnd;
    }

    SkTypeface* currentTypeface() const {
        return fCurrentTypeface;
    }
    hb_font_t* currentHBFont() const {
        return fCurrentHBFont;
    }
private:
    const char* fCurrent;
    const char* fEnd;
    sk_sp<SkFontMgr> fFallbackMgr;
    hb_font_t* fHBFont;
    sk_sp<SkTypeface> fTypeface;
    HBFont fFallbackHBFont;
    sk_sp<SkTypeface> fFallbackTypeface;
    hb_font_t* fCurrentHBFont;
    SkTypeface* fCurrentTypeface;
};

class RunIteratorQueue {
public:
    void insert(RunIterator* runIterator) {
        fRunIterators.insert(runIterator);
    }

    bool advanceRuns() {
        const RunIterator* leastRun = fRunIterators.peek();
        if (leastRun->atEnd()) {
            SkASSERT(this->allRunsAreAtEnd());
            return false;
        }
        const char* leastEnd = leastRun->endOfCurrentRun();
        RunIterator* currentRun = nullptr;
        SkDEBUGCODE(const char* previousEndOfCurrentRun);
        while ((currentRun = fRunIterators.peek())->endOfCurrentRun() <= leastEnd) {
            fRunIterators.pop();
            SkDEBUGCODE(previousEndOfCurrentRun = currentRun->endOfCurrentRun());
            currentRun->consume();
            SkASSERT(previousEndOfCurrentRun < currentRun->endOfCurrentRun());
            fRunIterators.insert(currentRun);
        }
        return true;
    }

    const char* endOfCurrentRun() const {
        return fRunIterators.peek()->endOfCurrentRun();
    }

private:
    bool allRunsAreAtEnd() const {
        for (int i = 0; i < fRunIterators.count(); ++i) {
            if (!fRunIterators.at(i)->atEnd()) {
                return false;
            }
        }
        return true;
    }

    static bool CompareRunIterator(RunIterator* const& a, RunIterator* const& b) {
        return *a < *b;
    }
    SkTDPQueue<RunIterator*, CompareRunIterator> fRunIterators;
};

struct ShapedGlyph {
    SkGlyphID fID;
    uint32_t fCluster;
    SkPoint fOffset;
    SkVector fAdvance;
    bool fMayLineBreakBefore;
    bool fMustLineBreakBefore;
    bool fHasVisual;
};
struct ShapedRun {
    ShapedRun(const char* utf8Start, const char* utf8End, int numGlyphs, const SkFont& font,
              UBiDiLevel level, std::unique_ptr<ShapedGlyph[]> glyphs)
        : fUtf8Start(utf8Start), fUtf8End(utf8End), fNumGlyphs(numGlyphs), fFont(font)
        , fLevel(level), fGlyphs(std::move(glyphs))
    {}

    const char* fUtf8Start;
    const char* fUtf8End;
    int fNumGlyphs;
    SkFont fFont;
    UBiDiLevel fLevel;
    std::unique_ptr<ShapedGlyph[]> fGlyphs;
    SkVector fAdvance = { 0, 0 };
};

static constexpr bool is_LTR(UBiDiLevel level) {
    return (level & 1) == 0;
}

static void append(SkShaper::RunHandler* handler, const SkShaper::RunHandler::RunInfo& runInfo,
                   const ShapedRun& run, int start, int end,
                   SkPoint* p) {
    unsigned len = end - start;

    const auto buffer = handler->newRunBuffer(runInfo, run.fFont, len, run.fUtf8End - run.fUtf8Start);
    SkASSERT(buffer.glyphs);
    SkASSERT(buffer.positions);

    if (buffer.utf8text) {
        memcpy(buffer.utf8text, run.fUtf8Start, run.fUtf8End - run.fUtf8Start);
    }

    for (unsigned i = 0; i < len; i++) {
        // Glyphs are in logical order, but output ltr since PDF readers seem to expect that.
        const ShapedGlyph& glyph = run.fGlyphs[is_LTR(run.fLevel) ? start + i : end - 1 - i];
        buffer.glyphs[i] = glyph.fID;
        buffer.positions[i] = SkPoint::Make(p->fX + glyph.fOffset.fX, p->fY - glyph.fOffset.fY);
        if (buffer.clusters) {
            buffer.clusters[i] = glyph.fCluster;
        }
        p->fX += glyph.fAdvance.fX;
        p->fY += glyph.fAdvance.fY;
    }
}

struct ShapedRunGlyphIterator {
    ShapedRunGlyphIterator(const SkTArray<ShapedRun>& origRuns)
        : fRuns(&origRuns), fRunIndex(0), fGlyphIndex(0)
    { }

    ShapedRunGlyphIterator(const ShapedRunGlyphIterator& that) = default;
    ShapedRunGlyphIterator& operator=(const ShapedRunGlyphIterator& that) = default;
    bool operator==(const ShapedRunGlyphIterator& that) const {
        return fRuns == that.fRuns &&
               fRunIndex == that.fRunIndex &&
               fGlyphIndex == that.fGlyphIndex;
    }
    bool operator!=(const ShapedRunGlyphIterator& that) const {
        return fRuns != that.fRuns ||
               fRunIndex != that.fRunIndex ||
               fGlyphIndex != that.fGlyphIndex;
    }

    ShapedGlyph* next() {
        const SkTArray<ShapedRun>& runs = *fRuns;
        SkASSERT(fRunIndex < runs.count());
        SkASSERT(fGlyphIndex < runs[fRunIndex].fNumGlyphs);

        ++fGlyphIndex;
        if (fGlyphIndex == runs[fRunIndex].fNumGlyphs) {
            fGlyphIndex = 0;
            ++fRunIndex;
            if (fRunIndex >= runs.count()) {
                return nullptr;
            }
        }
        return &runs[fRunIndex].fGlyphs[fGlyphIndex];
    }

    ShapedGlyph* current() {
        const SkTArray<ShapedRun>& runs = *fRuns;
        if (fRunIndex >= runs.count()) {
            return nullptr;
        }
        return &runs[fRunIndex].fGlyphs[fGlyphIndex];
    }

    const SkTArray<ShapedRun>* fRuns;
    int fRunIndex;
    int fGlyphIndex;
};

}  // namespace

struct SkShaper::Impl {
    HBFont fHarfBuzzFont;
    HBBuffer fBuffer;
    sk_sp<SkTypeface> fTypeface;
    ICUBrk fBreakIterator;
};

SkShaper::SkShaper(sk_sp<SkTypeface> tf) : fImpl(new Impl) {
    SkOnce once;
    once([] { SkLoadICU(); });

    fImpl->fTypeface = tf ? std::move(tf) : SkTypeface::MakeDefault();
    fImpl->fHarfBuzzFont = create_hb_font(fImpl->fTypeface.get());
    if (!fImpl->fHarfBuzzFont) {
        SkDebugf("create_hb_font failed!\n");
    }
    fImpl->fBuffer.reset(hb_buffer_create());
    SkASSERT(fImpl->fBuffer);

    UErrorCode status = U_ZERO_ERROR;
    fImpl->fBreakIterator.reset(ubrk_open(UBRK_LINE, "th", nullptr, 0, &status));
    if (U_FAILURE(status)) {
        SkDebugf("Could not create break iterator: %s", u_errorName(status));
        SK_ABORT("");
    }
}

SkShaper::~SkShaper() {}

bool SkShaper::good() const {
    return fImpl->fHarfBuzzFont &&
           fImpl->fBuffer &&
           fImpl->fTypeface &&
           fImpl->fBreakIterator;
}

SkPoint SkShaper::shape(RunHandler* handler,
                        const SkFont& srcFont,
                        const char* utf8,
                        size_t utf8Bytes,
                        bool leftToRight,
                        SkPoint point,
                        SkScalar width) const {
    sk_sp<SkFontMgr> fontMgr = SkFontMgr::RefDefault();
    SkASSERT(handler);
    UBiDiLevel defaultLevel = leftToRight ? UBIDI_DEFAULT_LTR : UBIDI_DEFAULT_RTL;
    //hb_script_t script = ...

    SkTArray<ShapedRun> runs;
{
    RunIteratorQueue runSegmenter;

    SkTLazy<BiDiRunIterator> maybeBidi(BiDiRunIterator::Make(utf8, utf8Bytes, defaultLevel));
    BiDiRunIterator* bidi = maybeBidi.getMaybeNull();
    if (!bidi) {
        return point;
    }
    runSegmenter.insert(bidi);

    hb_unicode_funcs_t* hbUnicode = hb_buffer_get_unicode_funcs(fImpl->fBuffer.get());
    SkTLazy<ScriptRunIterator> maybeScript(ScriptRunIterator::Make(utf8, utf8Bytes, hbUnicode));
    ScriptRunIterator* script = maybeScript.getMaybeNull();
    if (!script) {
        return point;
    }
    runSegmenter.insert(script);

    SkTLazy<FontRunIterator> maybeFont(FontRunIterator::Make(utf8, utf8Bytes,
                                                             fImpl->fTypeface,
                                                             fImpl->fHarfBuzzFont.get(),
                                                             std::move(fontMgr)));
    FontRunIterator* font = maybeFont.getMaybeNull();
    if (!font) {
        return point;
    }
    runSegmenter.insert(font);

    UBreakIterator& breakIterator = *fImpl->fBreakIterator;
    {
        UErrorCode status = U_ZERO_ERROR;
        UText utf8UText = UTEXT_INITIALIZER;
        utext_openUTF8(&utf8UText, utf8, utf8Bytes, &status);
        std::unique_ptr<UText, SkFunctionWrapper<UText*, UText, utext_close>> autoClose(&utf8UText);
        if (U_FAILURE(status)) {
            SkDebugf("Could not create utf8UText: %s", u_errorName(status));
            return point;
        }
        ubrk_setUText(&breakIterator, &utf8UText, &status);
        //utext_close(&utf8UText);
        if (U_FAILURE(status)) {
            SkDebugf("Could not setText on break iterator: %s", u_errorName(status));
            return point;
        }
    }

    const char* utf8Start = nullptr;
    const char* utf8End = utf8;
    while (runSegmenter.advanceRuns()) {
        utf8Start = utf8End;
        utf8End = runSegmenter.endOfCurrentRun();

        hb_buffer_t* buffer = fImpl->fBuffer.get();
        SkAutoTCallVProc<hb_buffer_t, hb_buffer_clear_contents> autoClearBuffer(buffer);
        hb_buffer_set_content_type(buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
        hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);

        // Add precontext.
        hb_buffer_add_utf8(buffer, utf8, utf8Start - utf8, utf8Start - utf8, 0);

        // Populate the hb_buffer directly with utf8 cluster indexes.
        const char* utf8Current = utf8Start;
        while (utf8Current < utf8End) {
            unsigned int cluster = utf8Current - utf8Start;
            hb_codepoint_t u = utf8_next(&utf8Current, utf8End);
            hb_buffer_add(buffer, u, cluster);
        }

        // Add postcontext.
        hb_buffer_add_utf8(buffer, utf8Current, utf8 + utf8Bytes - utf8Current, 0, 0);

        size_t utf8runLength = utf8End - utf8Start;
        if (!SkTFitsIn<int>(utf8runLength)) {
            SkDebugf("Shaping error: utf8 too long");
            return point;
        }
        hb_buffer_set_script(buffer, script->currentScript());
        hb_direction_t direction = is_LTR(bidi->currentLevel()) ? HB_DIRECTION_LTR:HB_DIRECTION_RTL;
        hb_buffer_set_direction(buffer, direction);
        // TODO: language
        hb_buffer_guess_segment_properties(buffer);
        // TODO: features
        if (!font->currentHBFont()) {
            continue;
        }
        hb_shape(font->currentHBFont(), buffer, nullptr, 0);
        unsigned len = hb_buffer_get_length(buffer);
        if (len == 0) {
            continue;
        }

        if (direction == HB_DIRECTION_RTL) {
            // Put the clusters back in logical order.
            // Note that the advances remain ltr.
            hb_buffer_reverse(buffer);
        }
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, nullptr);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, nullptr);

        if (!SkTFitsIn<int>(len)) {
            SkDebugf("Shaping error: too many glyphs");
            return point;
        }

        SkFont runFont(srcFont);
        runFont.setTypeface(sk_ref_sp(font->currentTypeface()));
        ShapedRun& run = runs.emplace_back(utf8Start, utf8End, len, runFont, bidi->currentLevel(),
                                           std::unique_ptr<ShapedGlyph[]>(new ShapedGlyph[len]));
        int scaleX, scaleY;
        hb_font_get_scale(font->currentHBFont(), &scaleX, &scaleY);
        double textSizeY = run.fFont.getSize() / scaleY;
        double textSizeX = run.fFont.getSize() / scaleX * run.fFont.getScaleX();
        SkVector runAdvance = { 0, 0 };
        for (unsigned i = 0; i < len; i++) {
            ShapedGlyph& glyph = run.fGlyphs[i];
            glyph.fID = info[i].codepoint;
            glyph.fCluster = info[i].cluster;
            glyph.fOffset.fX = pos[i].x_offset * textSizeX;
            glyph.fOffset.fY = pos[i].y_offset * textSizeY;
            glyph.fAdvance.fX = pos[i].x_advance * textSizeX;
            glyph.fAdvance.fY = pos[i].y_advance * textSizeY;
            glyph.fHasVisual = true; //!font->currentTypeface()->glyphBoundsAreZero(glyph.fID);
            //info->mask safe_to_break;
            glyph.fMustLineBreakBefore = false;

            runAdvance += glyph.fAdvance;
        }
        run.fAdvance = runAdvance;

        int32_t clusterOffset = utf8Start - utf8;
        uint32_t previousCluster = 0xFFFFFFFF;
        for (unsigned i = 0; i < len; ++i) {
            ShapedGlyph& glyph = run.fGlyphs[i];
            int32_t glyphCluster = glyph.fCluster + clusterOffset;
            int32_t breakIteratorCurrent = ubrk_current(&breakIterator);
            while (breakIteratorCurrent != UBRK_DONE &&
                   breakIteratorCurrent < glyphCluster)
            {
                breakIteratorCurrent = ubrk_next(&breakIterator);
            }
            glyph.fMayLineBreakBefore = glyph.fCluster != previousCluster &&
                                        breakIteratorCurrent == glyphCluster;
            previousCluster = glyph.fCluster;
        }
    }
}

// Iterate over the glyphs in logical order to mark line endings.
{
    SkScalar widthSoFar = 0;
    bool previousBreakValid = false; // Set when previousBreak is set to a valid candidate break.
    bool canAddBreakNow = false; // Disallow line breaks before the first glyph of a run.
    ShapedRunGlyphIterator previousBreak(runs);
    ShapedRunGlyphIterator glyphIterator(runs);
    while (ShapedGlyph* glyph = glyphIterator.current()) {
        if (canAddBreakNow && glyph->fMayLineBreakBefore) {
            previousBreakValid = true;
            previousBreak = glyphIterator;
        }
        SkScalar glyphWidth = glyph->fAdvance.fX;
        // TODO: if the glyph is non-visible it can be added.
        if (widthSoFar + glyphWidth < width) {
            widthSoFar += glyphWidth;
            glyphIterator.next();
            canAddBreakNow = true;
            continue;
        }

        // TODO: for both of these emergency break cases
        // don't break grapheme clusters and pull in any zero width or non-visible
        if (widthSoFar == 0) {
            // Adding just this glyph is too much, just break with this glyph
            glyphIterator.next();
            previousBreak = glyphIterator;
        } else if (!previousBreakValid) {
            // No break opportunity found yet, just break without this glyph
            previousBreak = glyphIterator;
        }
        glyphIterator = previousBreak;
        glyph = glyphIterator.current();
        if (glyph) {
            glyph->fMustLineBreakBefore = true;
        }
        widthSoFar = 0;
        previousBreakValid = false;
        canAddBreakNow = false;
    }
}

// Reorder the runs and glyphs per line and write them out.
    SkPoint currentPoint = point;
{
    ShapedRunGlyphIterator previousBreak(runs);
    ShapedRunGlyphIterator glyphIterator(runs);
    SkScalar maxAscent = 0;
    SkScalar maxDescent = 0;
    SkScalar maxLeading = 0;
    int previousRunIndex = -1;
    size_t lineIndex = 0;
    while (glyphIterator.current()) {
        int runIndex = glyphIterator.fRunIndex;
        int glyphIndex = glyphIterator.fGlyphIndex;
        ShapedGlyph* nextGlyph = glyphIterator.next();

        if (previousRunIndex != runIndex) {
            SkFontMetrics metrics;
            runs[runIndex].fFont.getMetrics(&metrics);
            maxAscent = SkTMin(maxAscent, metrics.fAscent);
            maxDescent = SkTMax(maxDescent, metrics.fDescent);
            maxLeading = SkTMax(maxLeading, metrics.fLeading);
            previousRunIndex = runIndex;
        }

        // Nothing can be written until the baseline is known.
        if (!(nextGlyph == nullptr || nextGlyph->fMustLineBreakBefore)) {
            continue;
        }

        currentPoint.fY -= maxAscent;

        int numRuns = runIndex - previousBreak.fRunIndex + 1;
        SkAutoSTMalloc<4, UBiDiLevel> runLevels(numRuns);
        for (int i = 0; i < numRuns; ++i) {
            runLevels[i] = runs[previousBreak.fRunIndex + i].fLevel;
        }
        SkAutoSTMalloc<4, int32_t> logicalFromVisual(numRuns);
        ubidi_reorderVisual(runLevels, numRuns, logicalFromVisual);

        for (int i = 0; i < numRuns; ++i) {
            int logicalIndex = previousBreak.fRunIndex + logicalFromVisual[i];

            int startGlyphIndex = (logicalIndex == previousBreak.fRunIndex)
                                ? previousBreak.fGlyphIndex
                                : 0;
            int endGlyphIndex = (logicalIndex == runIndex)
                              ? glyphIndex + 1
                              : runs[logicalIndex].fNumGlyphs;

            const auto& run = runs[logicalIndex];
            const RunHandler::RunInfo info = {
                lineIndex,
                run.fAdvance,
                maxAscent,
                maxDescent,
                maxLeading,
            };
            append(handler, info, run, startGlyphIndex, endGlyphIndex, &currentPoint);
        }

        currentPoint.fY += maxDescent + maxLeading;
        currentPoint.fX = point.fX;
        maxAscent = 0;
        maxDescent = 0;
        maxLeading = 0;
        previousRunIndex = -1;
        ++lineIndex;
        previousBreak = glyphIterator;
    }
}

    return currentPoint;
}
