//
//  File: %sys-string.h
//  Summary: {Definitions for REBSTR (e.g. WORD!) and REBUNI (e.g. STRING!)}
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// !!! R3-Alpha and Red would work with strings in their decoded form, in
// series of varying widths.  Ren-C's goal is to replace this with the idea
// of "UTF-8 everywhere", working with the strings as UTF-8 and only
// converting if the platform requires it for I/O (e.g. Windows):
//
// http://utf8everywhere.org/
//
// As a first step toward this goal, one place where strings were kept in
// UTF-8 form has been converted into series...the word table.  So for now,
// all REBSTR instances are for ANY-WORD!.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// The *current* implementation of Rebol's ANY-STRING! type has two different
// series widths that are used.  One is the BYTE_SIZED() series which encodes
// ASCII in the low bits, and Latin-1 extensions in the range 0x80 - 0xFF.
// So long as a codepoint can fit in this range, the string can be stored in
// single bytes:
//
// https://en.wikipedia.org/wiki/Latin-1_Supplement_(Unicode_block)
//
// (Note: This is not to be confused with the other "byte-width" encoding,
// which is UTF-8.  Rebol series routines are not set up to handle insertions
// or manipulations of UTF-8 encoded data in a Reb_Any_String payload at
// this time...it is a format used only in I/O.)
//
// The second format that is used puts codepoints into a 16-bit REBUNI-sized
// element.  If an insertion of a string or character into a byte sized
// string cannot be represented in 0xFF or lower, then the target string will
// be "widened"--doubling the storage space taken and requiring updating of
// the character data in memory.  At this time there are no "in-place"
// cases where a string is reduced from REBUNI to byte sized, but operations
// like Copy_String_At_Len() will scan a source string to see if a byte-size
// copy can be made from a REBUNI-sized one without loss of information.
//
// Byte-sized series are also used by the BINARY! datatype.  There is no
// technical difference between such series used as strings or used as binary,
// the difference comes from being marked REB_BINARY or REB_TEXT in the
// header of the value carrying the series.
//
// For easier type-correctness, the series macros are given with names BIN_XXX
// and UNI_XXX.  There aren't distinct data types for the series themselves,
// just REBSER* is used.  Hence BIN_LEN() and UNI_LEN() aren't needed as you
// could just use SER_LEN(), but it helps a bit for readability...and an
// assert is included to ensure the size matches up.
//


// R3-Alpha's concept was that all words got persistent integer values, which
// prevented garbage collection.  Ren-C only gives built-in words integer
// values--or SYMs--while others must be compared by pointers to their
// name or canon-name pointers.  A non-built-in symbol will return SYM_0 as
// its symbol, allowing it to fall through to defaults in case statements.
//
// Though it works fine for switch statements, it creates a problem if someone
// writes `VAL_WORD_SYM(a) == VAL_WORD_SYM(b)`, because all non-built-ins
// will appear to be equal.  It's a tricky enough bug to catch to warrant an
// extra check in C++ that disallows comparing SYMs with ==
//
#if !defined(NDEBUG) && defined(CPLUSPLUS_11)
    struct REBSYM;

    struct OPT_REBSYM { // can only be converted to REBSYM, no comparisons
        enum Reb_Symbol n;
        OPT_REBSYM (const REBSYM& sym);
        bool operator==(enum Reb_Symbol other) const {
            return n == other;
        }
        bool operator!=(enum Reb_Symbol other) const {
            return n != other;
        }

        bool operator==(OPT_REBSYM &&other) const;
        bool operator!=(OPT_REBSYM &&other) const;

        operator unsigned int() const {
            return cast(unsigned int, n);
        }
    };

    struct REBSYM { // acts like a REBOL_Symbol with no OPT_REBSYM compares
        enum Reb_Symbol n;
        REBSYM () {}
        REBSYM (int n) : n (cast(enum Reb_Symbol, n)) {}
        REBSYM (OPT_REBSYM opt_sym) : n (opt_sym.n) {}
        operator unsigned int() const {
            return cast(unsigned int, n);
        }
        bool operator>=(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n >= other;
        }
        bool operator<=(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n <= other;
        }
        bool operator>(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n > other;
        }
        bool operator<(enum Reb_Symbol other) const {
            assert(other != SYM_0);
            return n < other;
        }
        bool operator==(enum Reb_Symbol other) const {
            return n == other;
        }
        bool operator!=(enum Reb_Symbol other) const {
            return n != other;
        }
        bool operator==(REBSYM &other) const; // could be SYM_0!
        void operator!=(REBSYM &other) const; // could be SYM_0!
        bool operator==(const OPT_REBSYM &other) const; // could be SYM_0!
        void operator!=(const OPT_REBSYM &other) const; // could be SYM_0!
    };

    inline OPT_REBSYM::OPT_REBSYM(const REBSYM &sym) : n (sym.n) {}
#else
    typedef enum Reb_Symbol REBSYM;
    typedef enum Reb_Symbol OPT_REBSYM; // act sameas REBSYM in C build
#endif

inline static bool SAME_SYM_NONZERO(REBSYM a, REBSYM b) {
    assert(a != SYM_0 and b != SYM_0);
    return cast(REBCNT, a) == cast(REBCNT, b);
}


//=////////////////////////////////////////////////////////////////////////=//
//
//  REBSTR series for UTF-8 strings
//
//=////////////////////////////////////////////////////////////////////////=//
//
// The concept is that a SYM refers to one of the built-in words and can
// be used in C switch statements.  A canon STR is used to identify
// everything else.
//

inline static const char *STR_HEAD(REBSTR *str) {
    return cs_cast(BIN_HEAD(str));
}

inline static REBSTR *STR_CANON(REBSTR *str) {
    assert(SER_WIDE(str) == 1);
    while (NOT_SER_INFO(str, STRING_INFO_CANON))
        str = LINK(str).synonym; // circularly linked list
    return str;
}

inline static OPT_REBSYM STR_SYMBOL(REBSTR *str) {
    uint16_t sym = SECOND_UINT16(str->header);
    assert(sym == SECOND_UINT16(STR_CANON(str)->header));
    return cast(REBSYM, sym);
}

inline static size_t STR_SIZE(REBSTR *str) {
    return SER_LEN(str); // number of bytes in seris is series length, ATM
}

inline static REBSTR *Canon(REBSYM sym) {
    assert(cast(REBCNT, sym) != 0);
    assert(cast(REBCNT, sym) < SER_LEN(PG_Symbol_Canons));
    return *SER_AT(REBSTR*, PG_Symbol_Canons, cast(REBCNT, sym));
}

inline static bool SAME_STR(REBSTR *s1, REBSTR *s2) {
    if (s1 == s2)
        return true; // !!! does this check speed things up or not?
    return STR_CANON(s1) == STR_CANON(s2); // canon check, quite fast
}



//
// !!! UNI_XXX: Unicode string series macros !!! - Becoming Deprecated
//

inline static REBCNT UNI_LEN(REBSER *s) {
    assert(SER_WIDE(s) == sizeof(REBUNI));
    return SER_LEN(s);
}

inline static void SET_UNI_LEN(REBSER *s, REBCNT len) {
    assert(SER_WIDE(s) == sizeof(REBUNI));
    SET_SERIES_LEN(s, len);
}

#define UNI_AT(s,n) \
    AS_REBCHR(SER_AT(REBUNI, (s), (n)))

#define UNI_HEAD(s) \
    SER_HEAD(REBUNI, (s))

#define UNI_TAIL(s) \
    SER_TAIL(REBUNI, (s))

#define UNI_LAST(s) \
    SER_LAST(REBUNI, (s))

inline static void TERM_UNI_LEN(REBSER *s, REBCNT len) {
    SET_SERIES_LEN(s, len);
    *SER_AT(REBUNI, s, len) = '\0';
}

#define VAL_UNI_HEAD(v) \
    UNI_HEAD(VAL_SERIES(v))

#define VAL_UNI_TAIL(v) \
    UNI_TAIL(VAL_SERIES(v))

// This should be an updating operation, which may refresh the cache in the
// value.  It would look something like:
//
//     if (s->stamp == v->extra.utfcache.stamp)
//          return v->extra.utfcache.offset;
//     ...else calculate...
//    m_cast(REBVAL*, v)->extra.utfcache.stamp = s->stamp;
//    m_cast(REBVAL*, v)->extra.utfcache.offset = offset;
//
// One should thus always prefer to use VAL_UNI_AT() if possible, over trying
// to calculate a position from scratch.
//
inline static REBUNI *VAL_UNI_AT(const REBCEL *v) {
    return AS_REBUNI(UNI_AT(VAL_SERIES(v), VAL_INDEX(v)));
}

inline static REBSIZ VAL_SIZE_LIMIT_AT(
    REBCNT *length, // length in chars to end (including limit)
    const RELVAL *v,
    REBINT limit // -1 for no limit
){
    assert(ANY_STRING(v));

    REBCHR(const *) at = VAL_UNI_AT(v); // !!! update cache if needed
    REBCHR(const *) tail;

    if (limit == -1) {
        if (length != NULL)
            *length = VAL_LEN_AT(v);
        tail = VAL_UNI_TAIL(v); // byte count known (fast)
    }
    else {
        if (length != NULL)
            *length = limit;
        tail = at;
        for (; limit > 0; --limit)
            tail = NEXT_CHR(NULL, tail);
    }

    return (
        cast(const REBYTE*, AS_REBUNI(tail))
        - cast(const REBYTE*, AS_REBUNI(at))
    );
}


//
// Get or set a unit in a binary series or a string series.  Used by routines
// that do searching/etc. and want to apply to both BINARY! and ANY-STRING!,
// so it can't be converted to purely UTF-8 as written.
//
// !!! String logic will get more complex with UTF8-Everywhere; it may have to
// shift bytes out of the way.  Or it may not even be possible to set a
// character if there aren't characters established before it.  Any
// algorithm using these should likely instead be using the mold buffer to
// create new strings, if possible.
//

inline static REBUNI GET_ANY_CHAR(REBSER *s, REBCNT n) {
    return BYTE_SIZE(s) ? *BIN_AT(s, n) : *SER_AT(REBUNI, s, n);
}

inline static void SET_ANY_CHAR(REBSER *s, REBCNT n, REBUNI c) {
    if (BYTE_SIZE(s)) {
        assert(c <= 255);
        *BIN_AT(s, n) = c;
    }
    else
        *SER_AT(REBUNI, s, n) = c;
}

#define VAL_ANY_CHAR(v) \
    GET_ANY_CHAR(VAL_SERIES(v), VAL_INDEX(v))


//=////////////////////////////////////////////////////////////////////////=//
//
//  ANY-STRING! (uses `struct Reb_Any_Series`)
//
//=////////////////////////////////////////////////////////////////////////=//

#define Init_Text(v,s) \
    Init_Any_Series((v), REB_TEXT, (s))

#define Init_File(v,s) \
    Init_Any_Series((v), REB_FILE, (s))

#define Init_Email(v,s) \
    Init_Any_Series((v), REB_EMAIL, (s))

#define Init_Tag(v,s) \
    Init_Any_Series((v), REB_TAG, (s))

#define Init_Url(v,s) \
    Init_Any_Series((v), REB_URL, (s))


// R3-Alpha did not support unicode codepoints higher than 0xFFFF, because
// strings were only 1 or 2 bytes per character.  Until support for "astral
// plane" characters is added, this inline function traps large characters
// when strings are being scanned.  If a client wishes to handle them
// explicitly, use Back_Scan_UTF8_Char_Core().
//
// Though the machinery can decode a UTF32 32-bit codepoint, the interface
// uses a 16-bit REBUNI (due to that being all that Rebol supports at this
// time).  If a codepoint that won't fit in 16-bits is found, it will raise
// an error vs. return NULL.  This makes it clear that the problem is not
// with the data itself being malformed (the usual assumption of callers)
// but rather a limit of the implementation.
//
inline static const REBYTE *Back_Scan_UTF8_Char(
    REBUNI *out,
    const REBYTE *bp,
    REBSIZ *size
){
    unsigned long ch; // "UTF32" is defined as unsigned long
    const REBYTE *bp_new = Back_Scan_UTF8_Char_Core(&ch, bp, size);
    if (bp_new and ch > 0xFFFF)
        fail (Error_Codepoint_Too_High_Raw(rebInteger(ch)));
    *out = cast(REBUNI, ch);
    return bp_new;
}


// Basic string initialization from UTF8.  (Most clients should be using the
// rebStringXXX() APIs for this).  Note that these routines may fail() if the
// data they are given is not UTF-8.

inline static REBSER *Make_String_UTF8(const char *utf8)
{
    const bool crlf_to_lf = false;
    return Append_UTF8_May_Fail(NULL, utf8, strsize(utf8), crlf_to_lf);
}

inline static REBSER *Make_Sized_String_UTF8(const char *utf8, size_t size)
{
    const bool crlf_to_lf = false;
    return Append_UTF8_May_Fail(NULL, utf8, size, crlf_to_lf);
}


inline static REBINT Hash_String(REBSTR *str)
    { return Hash_UTF8(cb_cast(STR_HEAD(str)), STR_SIZE(str)); }

inline static REBINT First_Hash_Candidate_Slot(
    REBCNT *skip_out,
    REBCNT hash,
    REBCNT num_slots
){
    *skip_out = (hash & 0x0000FFFF) % num_slots;
    if (*skip_out == 0)
        *skip_out = 1;
    return (hash & 0x00FFFF00) % num_slots;
}


//
// Copy helpers
//

inline static REBSER *Copy_Sequence_At_Position(const REBVAL *v)
{
    return Copy_Sequence_At_Len_Extra(
        VAL_SERIES(v), VAL_INDEX(v), VAL_LEN_AT(v), 0
    );
}

inline static REBSER *Copy_Sequence_At_Len(
    REBSER *s,
    REBCNT index,
    REBCNT len
){
    return Copy_Sequence_At_Len_Extra(s, index, len, 0);
}


// This is a speculative routine, which is based on the idea that it will be
// common for UTF-8 anywhere strings to cache a bit saying whether they are
// in ASCII range and fixed size.  If this is the case, different algorithms
// might be applied, for instance a standard C qsort() to sort the characters.
//
inline static bool Is_String_ASCII(const RELVAL *str) {
    UNUSED(str);
    return false; // currently all strings are 16-bit REBUNI characters
}
