//
//  File: %t-typeset.c
//  Summary: "typeset datatype"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
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

#include "sys-core.h"


//
// symbol-to-typeset-bits mapping table
//
// NOTE: Order of symbols is important, because this is used to build a
// list of typeset word symbols ordered relative to their symbol #,
// which lays out the legal unbound WORD! values you can use during
// a MAKE TYPESET! (bound words will be looked up as variables to see
// if they contain a DATATYPE! or a typeset, but general reduction is
// not performed on the block passed in.)
//
// !!! Is it necessary for MAKE TYPESET! to allow unbound words at all,
// or should the typesets be required to be in bound variables?  Should
// clients be asked to pass in only datatypes and typesets, hence doing
// their own reduce before trying to make a typeset out of a block?
//
const struct {
    REBSYM sym;
    REBU64 bits;
} Typesets[] = {
    {SYM_ANY_VALUE_X, TS_VALUE},
    {SYM_ANY_WORD_X, TS_WORD},
    {SYM_ANY_PATH_X, TS_PATH},
    {SYM_ANY_NUMBER_X, TS_NUMBER},
    {SYM_ANY_SCALAR_X, TS_SCALAR},
    {SYM_ANY_SERIES_X, TS_SERIES},
    {SYM_ANY_STRING_X, TS_STRING},
    {SYM_ANY_CONTEXT_X, TS_CONTEXT},
    {SYM_ANY_ARRAY_X, TS_ARRAY},

    {SYM_0, 0}
};


//
//  CT_Typeset: C
//
REBINT CT_Typeset(const REBCEL *a, const REBCEL *b, REBINT mode)
{
    if (mode < 0) return -1;
    return EQUAL_TYPESET(a, b);
}


//
//  Startup_Typesets: C
//
// Create typeset variables that are defined above.
// For example: NUMBER is both integer and decimal.
// Add the new variables to the system context.
//
void Startup_Typesets(void)
{
    REBDSP dsp_orig = DSP;

    REBINT n;
    for (n = 0; Typesets[n].sym != 0; n++) {
        Init_Typeset(DS_PUSH(), Typesets[n].bits);

        Move_Value(
            Append_Context(Lib_Context, NULL, Canon(Typesets[n].sym)),
            DS_TOP
        );
    }

    // !!! Why does the system access the typesets through Lib_Context, vs.
    // using the Root_Typesets?
    //
    Root_Typesets = Init_Block(Alloc_Value(), Pop_Stack_Values(dsp_orig));

    REBSER *locker = nullptr;
    Ensure_Value_Frozen(Root_Typesets, locker);
}


//
//  Shutdown_Typesets: C
//
void Shutdown_Typesets(void)
{
    rebRelease(Root_Typesets);
    Root_Typesets = NULL;
}


//
//  Update_Typeset_Bits_Core: C
//
// This sets the bits in a bitset according to a block of datatypes.  There
// is special handling by which BAR! will set the "variadic" bit on the
// typeset, which is heeded by functions only.
//
// !!! R3-Alpha supported fixed word symbols for datatypes and typesets.
// Confusingly, this means that if you have said `word!: integer!` and use
// WORD!, you will get the integer type... but if WORD! is unbound then it
// will act as WORD!.  Also, is essentially having "keywords" and should be
// reviewed to see if anything actually used it.
//
bool Update_Typeset_Bits_Core(
    RELVAL *typeset,
    const RELVAL *head,
    REBSPC *specifier
) {
    assert(IS_TYPESET(typeset) or IS_PARAM(typeset));
    VAL_TYPESET_BITS(typeset) = 0;

    const RELVAL *maybe_word = head;
    for (; NOT_END(maybe_word); ++maybe_word) {
        REBCNT num_quotes = VAL_NUM_QUOTES(maybe_word);
        const REBCEL *unescaped = VAL_UNESCAPED(maybe_word);

        const RELVAL *item;
        if (CELL_KIND(unescaped) == REB_WORD) {
            item = Get_Opt_Var_May_Fail(unescaped, specifier);
            if (not item)
                fail (Error_No_Value_Core(maybe_word, specifier));
        }
        else
            item = maybe_word; // wasn't variable

        // Though MAKE ACTION! at its lowest level attempts to avoid any
        // keywords, there are native-optimized function generators that do
        // use them.  Since this code is shared by both, it may or may not
        // set typeset flags as a parameter.  Default to always for now.
        //
        const bool keywords = true;
        if (keywords and IS_TAG(item)) {
            if (0 == Compare_String_Vals(item, Root_Ellipsis_Tag, true)) {
                TYPE_SET(typeset, REB_TS_VARIADIC);
            }
            else if (0 == Compare_String_Vals(item, Root_End_Tag, true)) {
                TYPE_SET(typeset, REB_TS_ENDABLE);
            }
            else if (0 == Compare_String_Vals(item, Root_Blank_Tag, true)) {
                TYPE_SET(typeset, REB_TS_NOOP_IF_BLANK);
            }
            else if (0 == Compare_String_Vals(item, Root_Opt_Tag, true)) {
                //
                // !!! Review if this makes sense to allow with MAKE TYPESET!
                // instead of just function specs.
                //
                TYPE_SET(typeset, REB_MAX_NULLED);
            }
            else if (0 == Compare_String_Vals(item, Root_Skip_Tag, true)) {
                if (VAL_PARAM_CLASS(typeset) != REB_P_HARD_QUOTE)
                    fail ("Only hard-quoted parameters are <skip>-able");

                TYPE_SET(typeset, REB_TS_SKIPPABLE);
                TYPE_SET(typeset, REB_TS_ENDABLE); // skip => null
            }
        }
        else if (IS_DATATYPE(item)) {
            if (num_quotes == 0)
                TYPE_SET(typeset, VAL_TYPE_KIND(item));
            else {
                const REBCEL *cell = VAL_UNESCAPED(item);
                if (num_quotes > 1)
                   fail ("General type quoting not supported, use QUOTED!");

                if (VAL_TYPE_KIND(cell) == REB_WORD)
                    TYPE_SET(typeset, REB_TS_QUOTED_WORD);
                else if (VAL_TYPE_KIND(cell) == REB_PATH)
                    TYPE_SET(typeset, REB_TS_QUOTED_PATH);
                else
                    fail ("WORD!/PATH! quote typechecking only, use QUOTED!");
            }
        }
        else if (IS_TYPESET(item)) {
            if (num_quotes != 0)
                fail ("General typeset quoting not supported, use QUOTED!");

            VAL_TYPESET_BITS(typeset) |= VAL_TYPESET_BITS(item);
        }
        else if (IS_QUOTED(item)) {
            const REBCEL *cell = VAL_UNESCAPED(item);
            if (CELL_KIND(cell) != REB_DATATYPE)
                fail ("General typeset quoting not supported, use QUOTED!");

            if (VAL_TYPE_KIND(cell) == REB_WORD)
                TYPE_SET(typeset, REB_TS_QUOTED_WORD);
            else if (VAL_TYPE_KIND(cell) == REB_PATH)
                TYPE_SET(typeset, REB_TS_QUOTED_PATH);
            else
                fail ("WORD!/PATH! quote typechecking only, use QUOTED!");
        }
        else
            fail (Error_Invalid_Core(item, specifier));
    }

    return true;
}


//
//  MAKE_Typeset: C
//
REB_R MAKE_Typeset(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg)
{
    assert(kind == REB_TYPESET);
    UNUSED(kind);

    if (IS_TYPESET(arg))
        return Move_Value(out, arg);

    if (!IS_BLOCK(arg)) goto bad_make;

    Init_Typeset(out, 0);
    Update_Typeset_Bits_Core(out, VAL_ARRAY_AT(arg), VAL_SPECIFIER(arg));
    return out;

  bad_make:
    fail (Error_Bad_Make(REB_TYPESET, arg));
}


//
//  TO_Typeset: C
//
REB_R TO_Typeset(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg)
{
    return MAKE_Typeset(out, kind, arg);
}


//
//  Typeset_To_Array: C
//
// Converts typeset value to a block of datatypes, no order is guaranteed.
//
REBARR *Typeset_To_Array(const REBVAL *tset)
{
    REBDSP dsp_orig = DSP;

    REBINT n;
    for (n = 1; n < REB_MAX_NULLED; ++n) {
        if (TYPE_CHECK(tset, cast(enum Reb_Kind, n))) {
            if (n == REB_MAX_NULLED) {
                //
                // !!! A BLANK! value is currently supported in typesets to
                // indicate that they take optional values.  This may wind up
                // as a feature of MAKE ACTION! only.
                //
                Init_Blank(DS_PUSH());
            }
            else
                Init_Datatype(DS_PUSH(), cast(enum Reb_Kind, n));
        }
    }

    return Pop_Stack_Values(dsp_orig);
}


//
//  MF_Typeset: C
//
void MF_Typeset(REB_MOLD *mo, const REBCEL *v, bool form)
{
    REBINT n;

    if (not form) {
        Pre_Mold(mo, v);  // #[typeset! or make typeset!
        Append_Utf8_Codepoint(mo->series, '[');
    }

    // Convert bits to type name strings.  Note that "endability" and
    // "optionality" are not really good fits for things in a typeset, as no
    // "type" exists for their bits.  However, you can get them if you say
    // `TYPESETS OF` on an action.  This should be thought about.

    if (TYPE_CHECK(v, REB_0_END))
        Emit(mo, "<end> ");

    if (TYPE_CHECK(v, REB_MAX_NULLED))
        Emit(mo, "<opt> ");

    // !!! What about REB_TS_SKIPPABLE and other parameter properties, that
    // don't really fit into "types", but you can get with TYPESETS OF action?

    for (n = REB_0 + 1; n < REB_MAX; n++) {
        if (TYPE_CHECK(v, cast(enum Reb_Kind, n)))
            Emit(mo, "+DN ", SYM_DATATYPE_X, Canon(cast(REBSYM, n)));
    }
    Trim_Tail(mo->series, ' ');

    if (not form) {
        Append_Utf8_Codepoint(mo->series, ']');
        End_Mold(mo);
    }
}


//
//  REBTYPE: C
//
REBTYPE(Typeset)
{
    REBVAL *val = D_ARG(1);
    REBVAL *arg = D_ARGC > 1 ? D_ARG(2) : NULL;

    switch (VAL_WORD_SYM(verb)) {

    case SYM_FIND:
        if (not IS_DATATYPE(arg))
            fail (Error_Invalid(arg));

        if (TYPE_CHECK(val, VAL_TYPE_KIND(arg)))
            return Init_Bar(D_OUT);

        return nullptr;

    case SYM_INTERSECT:
    case SYM_UNION:
    case SYM_DIFFERENCE:
        if (IS_DATATYPE(arg)) {
            VAL_TYPESET_BITS(arg) = FLAGIT_KIND(VAL_TYPE(arg));
        }
        else if (not IS_TYPESET(arg))
            fail (Error_Invalid(arg));

        if (VAL_WORD_SYM(verb) == SYM_UNION)
            VAL_TYPESET_BITS(val) |= VAL_TYPESET_BITS(arg);
        else if (VAL_WORD_SYM(verb) == SYM_INTERSECT)
            VAL_TYPESET_BITS(val) &= VAL_TYPESET_BITS(arg);
        else {
            assert(VAL_WORD_SYM(verb) == SYM_DIFFERENCE);
            VAL_TYPESET_BITS(val) ^= VAL_TYPESET_BITS(arg);
        }
        RETURN (val);

    case SYM_COMPLEMENT:
        VAL_TYPESET_BITS(val) = ~VAL_TYPESET_BITS(val);
        RETURN (val);

    default:
        break;
    }

    fail (Error_Illegal_Action(REB_TYPESET, verb));
}
