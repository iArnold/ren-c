//
//  File: %c-function.c
//  Summary: "support for functions, actions, and routines"
//  Section: core
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
//  List_Func_Words: C
//
// Return a block of function words, unbound.
// Note: skips 0th entry.
//
REBARR *List_Func_Words(const RELVAL *func, REBOOL pure_locals)
{
    REBARR *array = Make_Array(VAL_FUNC_NUM_PARAMS(func));
    REBVAL *param = VAL_FUNC_PARAMS_HEAD(func);

    for (; NOT_END(param); param++) {
        enum Reb_Kind kind;

        switch (VAL_PARAM_CLASS(param)) {
        case PARAM_CLASS_NORMAL:
            kind = REB_WORD;
            break;

        case PARAM_CLASS_TIGHT:
            kind = REB_ISSUE;
            break;

        case PARAM_CLASS_REFINEMENT:
            kind = REB_REFINEMENT;
            break;

        case PARAM_CLASS_HARD_QUOTE:
            kind = REB_GET_WORD;
            break;

        case PARAM_CLASS_SOFT_QUOTE:
            kind = REB_LIT_WORD;
            break;

        case PARAM_CLASS_LOCAL:
        case PARAM_CLASS_RETURN: // "magic" local - prefilled invisibly
        case PARAM_CLASS_LEAVE: // "magic" local - prefilled invisibly
            if (!pure_locals)
                continue; // treat as invisible, e.g. for WORDS-OF

            kind = REB_SET_WORD;
            break;

        default:
            assert(FALSE);
            DEAD_END;
        }

        Init_Any_Word(
            Alloc_Tail_Array(array), kind, VAL_PARAM_SPELLING(param)
        );
    }

    return array;
}


//
//  List_Func_Typesets: C
//
// Return a block of function arg typesets.
// Note: skips 0th entry.
//
REBARR *List_Func_Typesets(REBVAL *func)
{
    REBARR *array = Make_Array(VAL_FUNC_NUM_PARAMS(func));
    REBVAL *typeset = VAL_FUNC_PARAMS_HEAD(func);

    for (; NOT_END(typeset); typeset++) {
        assert(IS_TYPESET(typeset));

        REBVAL *value = Alloc_Tail_Array(array);
        Move_Value(value, typeset);

        // !!! It's already a typeset, but this will clear out the header
        // bits.  This may not be desirable over the long run (what if
        // a typeset wishes to encode hiddenness, protectedness, etc?)
        //
        VAL_RESET_HEADER(value, REB_TYPESET);
    }

    return array;
}


enum Reb_Spec_Mode {
    SPEC_MODE_NORMAL, // words are arguments
    SPEC_MODE_LOCAL, // words are locals
    SPEC_MODE_WITH // words are "extern"
};


//
//  Make_Paramlist_Managed_May_Fail: C
//
// Check function spec of the form:
//
//     ["description" arg "notes" [type! type2! ...] /ref ...]
//
// !!! The spec language was not formalized in R3-Alpha.  Strings were left
// in and it was HELP's job (and any other clients) to make sense of it, e.g.:
//
//     [foo [type!] {doc string :-)}]
//     [foo {doc string :-/} [type!]]
//     [foo {doc string1 :-/} {doc string2 :-(} [type!]]
//
// Ren-C breaks this into two parts: one is the mechanical understanding of
// MAKE FUNCTION! for parameters in the evaluator.  Then it is the job
// of a generator to tag the resulting function with a "meta object" with any
// descriptions.  As a proxy for the work of a usermode generator, this
// routine tries to fill in FUNCTION-META (see %sysobj.r) as well as to
// produce a paramlist suitable for the function.
//
// Note a "true local" (indicated by a set-word) is considered to be tacit
// approval of wanting a definitional return by the generator.  This helps
// because Red's model for specifying returns uses a SET-WORD!
//
//     func [return: [integer!] {returns an integer}]
//
// In Ren/C's case it just means you want a local called return, but the
// generator will be "initializing it with a definitional return" for you.
// You don't have to use it if you don't want to...and may overwrite the
// variable.  But it won't be a void at the start.
//
REBARR *Make_Paramlist_Managed_May_Fail(
    const REBVAL *spec,
    REBFLGS flags
) {
    assert(ANY_ARRAY(spec));

    REBUPT header_bits = 0;

#if !defined(NDEBUG)
    //
    // Debug builds go ahead and include a RETURN field and hang onto the
    // typeset for fake returns (e.g. natives).  But they make a note that
    // they are doing this, which helps know what the actual size of the
    // frame would be in a release build (e.g. for a FRM_CELL() assert)
    //
    if (flags & MKF_FAKE_RETURN) {
        header_bits |= FUNC_FLAG_RETURN_DEBUG;
        flags &= ~MKF_FAKE_RETURN;
        assert(NOT(flags & MKF_RETURN));
        flags |= MKF_RETURN;
    }
#endif

    REBDSP dsp_orig = DSP;
    assert(DS_TOP == DS_AT(dsp_orig));

    REBDSP definitional_return_dsp = 0;
    REBDSP definitional_leave_dsp = 0;

    // As we go through the spec block, we push TYPESET! BLOCK! STRING! triples.
    // These will be split out into separate arrays after the process is done.
    // The first slot of the paramlist needs to be the function canon value,
    // while the other two first slots need to be rootkeys.  Get the process
    // started right after a BLOCK! so it's willing to take a string for
    // the function description--it will be extracted from the slot before
    // it is turned into a rootkey for param_notes.
    //
    DS_PUSH_TRASH; // paramlist[0] (will become FUNCTION! canon value)
    Init_Unreadable_Blank(DS_TOP);
    DS_PUSH(EMPTY_BLOCK); // param_types[0] (to be OBJECT! canon value, if any)
    DS_PUSH(EMPTY_STRING); // param_notes[0] (holds description, then canon)

    REBOOL has_description = FALSE;
    REBOOL has_types = FALSE;
    REBOOL has_notes = FALSE;

    enum Reb_Spec_Mode mode = SPEC_MODE_NORMAL;

    REBOOL refinement_seen = FALSE;

    const RELVAL *value = VAL_ARRAY_AT(spec);

    while (NOT_END(value)) {
        const RELVAL *item = value; // "faked", e.g. <return> => RETURN:
        ++value; // go ahead and consume next

    //=//// STRING! FOR FUNCTION DESCRIPTION OR PARAMETER NOTE ////////////=//

        if (IS_STRING(item)) {
            //
            // Consider `[<with> some-extern "description of that extern"]` to
            // be purely commentary for the implementation, and don't include
            // it in the meta info.
            //
            if (mode == SPEC_MODE_WITH)
                continue;

            if (IS_TYPESET(DS_TOP))
                DS_PUSH(EMPTY_BLOCK); // need a block to be in position

            if (IS_BLOCK(DS_TOP)) { // we're in right spot to push notes/title
                DS_PUSH_TRASH;
                Init_String(
                    DS_TOP,
                    Copy_String_Slimming(VAL_SERIES(item), VAL_INDEX(item), -1)
                );
            }
            else {
                assert(IS_STRING(DS_TOP));

                // !!! A string was already pushed.  Should we append?
                //
                Init_String(
                    DS_TOP,
                    Copy_String_Slimming(VAL_SERIES(item), VAL_INDEX(item), -1)
                );
            }

            if (DS_TOP == DS_AT(dsp_orig + 3))
                has_description = TRUE;
            else
                has_notes = TRUE;

            continue;
        }

    //=//// TOP-LEVEL SPEC TAGS LIKE <local>, <with> etc. /////////////////=//

        if (IS_TAG(item) && (flags & MKF_KEYWORDS)) {
            if (0 == Compare_String_Vals(item, Root_With_Tag, TRUE)) {
                mode = SPEC_MODE_WITH;
            }
            else if (0 == Compare_String_Vals(item, Root_Local_Tag, TRUE)) {
                mode = SPEC_MODE_LOCAL;
            }
            else
                fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));

            continue;
        }

    //=//// BLOCK! OF TYPES TO MAKE TYPESET FROM (PLUS PARAMETER TAGS) ////=//

        if (IS_BLOCK(item)) {
            if (IS_BLOCK(DS_TOP)) // two blocks of types!
                fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));

            // You currently can't say `<local> x [integer!]`, because they
            // are always void when the function runs.  You can't say
            // `<with> x [integer!]` because "externs" don't have param slots
            // to store the type in.
            //
            // !!! A type constraint on a <with> parameter might be useful,
            // though--and could be achieved by adding a type checker into
            // the body of the function.  However, that would be more holistic
            // than this generation of just a paramlist.  Consider for future.
            //
            if (mode != SPEC_MODE_NORMAL)
                fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));

            // Save the block for parameter types.
            //
            REBVAL *typeset;
            if (IS_TYPESET(DS_TOP)) {
                REBSPC *derived = Derive_Specifier(VAL_SPECIFIER(spec), item);
                DS_PUSH_TRASH;
                Init_Block(
                    DS_TOP,
                    Copy_Array_At_Deep_Managed(
                        VAL_ARRAY(item),
                        VAL_INDEX(item),
                        derived
                    )
                );

                typeset = DS_TOP - 1; // volatile if you DS_PUSH!
            }
            else {
                assert(IS_STRING(DS_TOP)); // !!! are blocks after notes good?

                if (IS_BLANK_RAW(DS_TOP - 2)) {
                    //
                    // No typesets pushed yet, so this is a block before any
                    // parameters have been named.  This was legal in Rebol2
                    // for e.g. `func [[catch] x y][...]`, and R3-Alpha
                    // ignored it.  Ren-C only tolerates this in <r3-legacy>,
                    // (with the tolerance implemented in compatibility FUNC)
                    //
                    fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));
                }

                assert(IS_TYPESET(DS_TOP - 2));
                typeset = DS_TOP - 2;

                assert(IS_BLOCK(DS_TOP - 1));
                if (VAL_ARRAY(DS_TOP - 1) != EMPTY_ARRAY)
                    fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));

                REBSPC *derived = Derive_Specifier(VAL_SPECIFIER(spec), item);
                Init_Block(
                    DS_TOP - 1,
                    Copy_Array_At_Deep_Managed(
                        VAL_ARRAY(item),
                        VAL_INDEX(item),
                        derived
                    )
                );
            }

            // Turn block into typeset for parameter at current index.
            // Leaves VAL_TYPESET_SYM as-is.
            //
            REBSPC *derived = Derive_Specifier(VAL_SPECIFIER(spec), item);
            Update_Typeset_Bits_Core(
                typeset,
                VAL_ARRAY_HEAD(item),
                derived
            );

            // Refinements and refinement arguments cannot be specified as
            // <opt>.  Although refinement arguments may be void, they are
            // not "passed in" that way...the refinement is inactive.
            //
            if (refinement_seen) {
                if (TYPE_CHECK(typeset, REB_MAX_VOID))
                    fail (Error_Refinement_Arg_Opt_Raw());
            }

            has_types = TRUE;
            continue;
        }

    //=//// ANY-WORD! PARAMETERS THEMSELVES (MAKE TYPESETS w/SYMBOL) //////=//

        if (!ANY_WORD(item))
            fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));

        // !!! If you say [<with> x /foo y] the <with> terminates and a
        // refinement is started.  Same w/<local>.  Is this a good idea?
        // Note that historically, help hides any refinements that appear
        // behind a /local, but this feature has no parallel in Ren-C.
        //
        if (mode != SPEC_MODE_NORMAL) {
            if (IS_REFINEMENT(item)) {
                mode = SPEC_MODE_NORMAL;
            }
            else if (!IS_WORD(item) && !IS_SET_WORD(item))
                fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));
        }

        REBSTR *canon = VAL_WORD_CANON(item);

        // In rhythm of TYPESET! BLOCK! STRING! we want to be on a string spot
        // at the time of the push of each new typeset.
        //
        if (IS_TYPESET(DS_TOP))
            DS_PUSH(EMPTY_BLOCK);
        if (IS_BLOCK(DS_TOP))
            DS_PUSH(EMPTY_STRING);
        assert(IS_STRING(DS_TOP));

        // By default allow "all datatypes but function and void".  Note that
        // since void isn't a "datatype" the use of the REB_MAX_VOID bit is for
        // expedience.  Also that there are two senses of void signal...the
        // typeset REB_MAX_VOID represents <opt> sense, not the <end> sense,
        // which is encoded by TYPESET_FLAG_ENDABLE.
        //
        // We do not canonize the saved symbol in the paramlist, see #2258.
        //
        DS_PUSH_TRASH;
        REBVAL *typeset = DS_TOP; // volatile if you DS_PUSH!
        Init_Typeset(
            typeset,
            (flags & MKF_ANY_VALUE)
                ? ALL_64
                : ALL_64 & ~(FLAGIT_KIND(REB_MAX_VOID) | FLAGIT_KIND(REB_FUNCTION)),
            VAL_WORD_SPELLING(item)
        );

        // All these would cancel a definitional return (leave has same idea):
        //
        //     func [return [integer!]]
        //     func [/refinement return]
        //     func [<local> return]
        //     func [<with> return]
        //
        // ...although `return:` is explicitly tolerated ATM for compatibility
        // (despite violating the "pure locals are NULL" premise)
        //
        if (STR_SYMBOL(canon) == SYM_RETURN && NOT(flags & MKF_LEAVE)) {
            assert(definitional_return_dsp == 0);
            if (IS_SET_WORD(item))
                definitional_return_dsp = DSP; // RETURN: explicitly tolerated
            else
                flags &= ~(MKF_RETURN | MKF_FAKE_RETURN);
        }
        else if (
            STR_SYMBOL(canon) == SYM_LEAVE
            && NOT(flags & (MKF_RETURN | MKF_FAKE_RETURN))
        ) {
            assert(definitional_leave_dsp == 0);
            if (IS_SET_WORD(item))
                definitional_leave_dsp = DSP; // LEAVE: explicitly tolerated
            else
                flags &= ~MKF_LEAVE;
        }

        if (mode == SPEC_MODE_WITH && !IS_SET_WORD(item)) {
            //
            // Because FUNC does not do any locals gathering by default, the
            // main purpose of <with> is for instructing it not to do the
            // definitional returns.  However, it also makes changing between
            // FUNC and FUNCTION more fluid.
            //
            // !!! If you write something like `func [x <with> x] [...]` that
            // should be sanity checked with an error...TBD.
            //
            DS_DROP; // forge the typeset, used in `definitional_return` case
            continue;
        }

        switch (VAL_TYPE(item)) {
        case REB_WORD:
            assert(mode != SPEC_MODE_WITH); // should have continued...
            INIT_VAL_PARAM_CLASS(
                typeset,
                (mode == SPEC_MODE_LOCAL)
                    ? PARAM_CLASS_LOCAL
                    : PARAM_CLASS_NORMAL
            );
            break;

        case REB_GET_WORD:
            assert(mode == SPEC_MODE_NORMAL);
            INIT_VAL_PARAM_CLASS(typeset, PARAM_CLASS_HARD_QUOTE);
            break;

        case REB_LIT_WORD:
            assert(mode == SPEC_MODE_NORMAL);
            INIT_VAL_PARAM_CLASS(typeset, PARAM_CLASS_SOFT_QUOTE);
            break;

        case REB_REFINEMENT:
            refinement_seen = TRUE;
            INIT_VAL_PARAM_CLASS(typeset, PARAM_CLASS_REFINEMENT);

            // !!! The typeset bits of a refinement are not currently used.
            // They are checked for TRUE or FALSE but this is done literally
            // by the code.  This means that every refinement has some spare
            // bits available in it for another purpose.
            break;

        case REB_SET_WORD:
            // tolerate as-is if in <local> or <with> mode...
            INIT_VAL_PARAM_CLASS(typeset, PARAM_CLASS_LOCAL);
            //
            // !!! Typeset bits of pure locals also not currently used,
            // though definitional return should be using it for the return
            // type of the function.
            //
            break;

        case REB_ISSUE:
            //
            // !!! Because of their role in the preprocessor in Red, and a
            // likely need for a similar behavior in Rebol, ISSUE! might not
            // be the ideal choice to mark tight parameters.
            //
            assert(mode == SPEC_MODE_NORMAL);
            INIT_VAL_PARAM_CLASS(typeset, PARAM_CLASS_TIGHT);
            break;

        default:
            fail (Error_Bad_Func_Def_Core(item, VAL_SPECIFIER(spec)));
        }
    }

    // Go ahead and flesh out the TYPESET! BLOCK! STRING! triples.
    //
    if (IS_TYPESET(DS_TOP))
        DS_PUSH(EMPTY_BLOCK);
    if (IS_BLOCK(DS_TOP))
        DS_PUSH(EMPTY_STRING);
    assert((DSP - dsp_orig) % 3 == 0); // must be a multiple of 3

    // Definitional RETURN and LEAVE slots must have their argument values
    // fulfilled with FUNCTION! values specific to the function being called
    // on *every instantiation*.  They are marked with special parameter
    // classes to avoid needing to separately do canon comparison of their
    // symbols to find them.  In addition, since RETURN's typeset holds
    // types that need to be checked at the end of the function run, it
    // is moved to a predictable location: last slot of the paramlist.
    //
    // Note: Trying to take advantage of the "predictable first position"
    // by swapping is not legal, as the first argument's position matters
    // in the ordinary arity of calling.

    if (flags & MKF_LEAVE) {
        if (definitional_leave_dsp == 0) { // no LEAVE: pure local explicit
            REBSTR *canon_leave = Canon(SYM_LEAVE);

            DS_PUSH_TRASH;
            Init_Typeset(DS_TOP, FLAGIT_KIND(REB_MAX_VOID), canon_leave);
            INIT_VAL_PARAM_CLASS(DS_TOP, PARAM_CLASS_LEAVE);
            definitional_leave_dsp = DSP;

            DS_PUSH(EMPTY_BLOCK);
            DS_PUSH(EMPTY_STRING);
        }
        else {
            REBVAL *definitional_leave = DS_AT(definitional_leave_dsp);
            assert(VAL_PARAM_CLASS(definitional_leave) == PARAM_CLASS_LOCAL);
            INIT_VAL_PARAM_CLASS(definitional_leave, PARAM_CLASS_LEAVE);
        }
        header_bits |= FUNC_FLAG_LEAVE;
    }

    if (flags & MKF_RETURN) {
        if (definitional_return_dsp == 0) { // no RETURN: pure local explicit
            REBSTR *canon_return = Canon(SYM_RETURN);

            // !!! The current experiment for dealing with default type
            // checking on definitional returns is to be somewhat restrictive
            // if there are *any* documentation notes or typesets on the
            // function.  Hence:
            //
            //     >> foo: func [x] [] ;-- no error, void return allowed
            //     >> foo: func [{a} x] [] ;-- will error, can't return void
            //
            // The idea is that if any effort has been expended on documenting
            // the interface at all, it has some "public" component...so
            // problems like leaking arbitrary values (vs. using PROC) are
            // more likely to be relevant.  Whereas no effort indicates a
            // likely more ad-hoc experimentation.
            //
            // (A "strict" mode, selectable per module, could control this and
            // other settings.  But the goal is to attempt to define something
            // that is as broadly usable as possible.)
            //
            DS_PUSH_TRASH;
            Init_Typeset(
                DS_TOP,
                (flags & MKF_ANY_VALUE)
                || NOT(has_description || has_types || has_notes)
                    ? ALL_64
                    : ALL_64 & ~(
                        FLAGIT_KIND(REB_MAX_VOID) | FLAGIT_KIND(REB_FUNCTION)
                    ),
                canon_return
            );
            INIT_VAL_PARAM_CLASS(DS_TOP, PARAM_CLASS_RETURN);
            definitional_return_dsp = DSP;

            DS_PUSH(EMPTY_BLOCK);
            DS_PUSH(EMPTY_STRING);
            // no need to move it--it's already at the tail position
        }
        else {
            REBVAL *definitional_return = DS_AT(definitional_return_dsp);
            assert(VAL_PARAM_CLASS(definitional_return) == PARAM_CLASS_LOCAL);
            INIT_VAL_PARAM_CLASS(definitional_return, PARAM_CLASS_RETURN);

            // definitional_return handled specially when paramlist copied
            // off of the stack...
        }
        header_bits |= FUNC_FLAG_RETURN;
    }

    // Slots, which is length +1 (includes the rootvar or rootparam)
    //
    REBCNT num_slots = (DSP - dsp_orig) / 3;

    // If we pushed a typeset for a return and it's a native, it actually
    // doesn't want a RETURN: key in the frame in release builds.  We'll omit
    // from the copy.
    //
    if (definitional_return_dsp != 0 && (flags & MKF_FAKE_RETURN))
        --num_slots;

    // There should be no more pushes past this point, so a stable pointer
    // into the stack for the definitional return can be found.
    //
    REBVAL *definitional_return =
        definitional_return_dsp == 0
            ? NULL
            : DS_AT(definitional_return_dsp);

    // Must make the function "paramlist" even if "empty", for identity.
    // Also make sure the parameter list does not expand.
    //
    // !!! Expanding the parameter list might be part of an advanced feature
    // under the hood in the future, but users should not themselves grow
    // function frames by appending to them.
    //
    REBARR *paramlist = Make_Array_Core(
        num_slots,
        ARRAY_FLAG_PARAMLIST | SERIES_FLAG_FIXED_SIZE
    );

    // In order to use this paramlist as a ->phase in a frame below, it must
    // have a valid facade so CTX_KEYLIST() will work.  The Make_Function()
    // calls that provide facades all currently build the full function before
    // trying to add any meta information that includes frames, so they do
    // not have to do this.
    //
    LINK(paramlist).facade = paramlist;

    if (TRUE) {
        RELVAL *dest = ARR_HEAD(paramlist); // canon function value
        VAL_RESET_HEADER(dest, REB_FUNCTION);
        SET_VAL_FLAGS(dest, header_bits);
        dest->payload.function.paramlist = paramlist;
        INIT_BINDING(dest, UNBOUND);
        ++dest;

        // We want to check for duplicates and a Binder can be used for that
        // purpose--but note that a fail() cannot happen while binders are
        // in effect UNLESS the BUF_COLLECT contains information to undo it!
        // There's no BUF_COLLECT here, so don't fail while binder in effect.
        //
        // (This is why we wait until the parameter list gathering process
        // is over to do the duplicate checks--it can fail.)
        //
        struct Reb_Binder binder;
        INIT_BINDER(&binder);

        REBSTR *duplicate = NULL;

        REBVAL *src = DS_AT(dsp_orig + 1) + 3;

        for (; src <= DS_TOP; src += 3) {
            assert(IS_TYPESET(src));
            if (!Try_Add_Binder_Index(&binder, VAL_PARAM_CANON(src), 1020))
                duplicate = VAL_PARAM_SPELLING(src);

            if (definitional_return && src == definitional_return)
                continue;

            Move_Value(dest, src);
            ++dest;
        }

        if (definitional_return) {
            if (flags & MKF_FAKE_RETURN) {
                //
                // This is where you don't actually want a RETURN key in the
                // function frame (e.g. because it's native code and would be
                // wasteful and unused).
                //
                // !!! The debug build uses real returns, not fake ones.
                // This means actions and natives have an extra slot.
                //
            }
            else {
                assert(flags & MKF_RETURN);
                Move_Value(dest, definitional_return);
                ++dest;
            }
        }

        // Must remove binder indexes for all words, even if about to fail
        //
        src = DS_AT(dsp_orig + 1) + 3;
        for (; src <= DS_TOP; src += 3, ++dest) {
            if (
                Remove_Binder_Index_Else_0(&binder, VAL_PARAM_CANON(src))
                == 0
            ){
                assert(duplicate != NULL);
            }
        }

        SHUTDOWN_BINDER(&binder);

        if (duplicate != NULL) {
            DECLARE_LOCAL (word);
            Init_Word(word, duplicate);
            fail (Error_Dup_Vars_Raw(word));
        }

        TERM_ARRAY_LEN(paramlist, num_slots);
        MANAGE_ARRAY(paramlist);
    }

    //=///////////////////////////////////////////////////////////////////=//
    //
    // BUILD META INFORMATION OBJECT (IF NEEDED)
    //
    //=///////////////////////////////////////////////////////////////////=//

    // !!! See notes on FUNCTION-META in %sysobj.r

    REBCTX *meta = NULL;

    if (has_description || has_types || has_notes) {
        meta = Copy_Context_Shallow(VAL_CONTEXT(Root_Function_Meta));
        MANAGE_ARRAY(CTX_VARLIST(meta));
    }

    MISC(paramlist).meta = meta;

    // If a description string was gathered, it's sitting in the first string
    // slot, the third cell we pushed onto the stack.  Extract it if so.
    //
    if (has_description) {
        assert(IS_STRING(DS_AT(dsp_orig + 3)));
        Move_Value(
            CTX_VAR(meta, STD_FUNCTION_META_DESCRIPTION),
            DS_AT(dsp_orig + 3)
        );
    }
    else if (meta)
        Init_Void(CTX_VAR(meta, STD_FUNCTION_META_DESCRIPTION));

    // Only make `parameter-types` if there were blocks in the spec
    //
    if (NOT(has_types)) {
        if (meta) {
            Init_Void(CTX_VAR(meta, STD_FUNCTION_META_PARAMETER_TYPES));
            Init_Void(CTX_VAR(meta, STD_FUNCTION_META_RETURN_TYPE));
        }
    }
    else {
        REBARR *types_varlist = Make_Array_Core(
            num_slots, ARRAY_FLAG_VARLIST
        );
        MISC(types_varlist).meta = NULL; // GC sees this, must initialize
        INIT_CTX_KEYLIST_SHARED(CTX(types_varlist), paramlist);

        REBVAL *dest = SINK(ARR_HEAD(types_varlist)); // "rootvar"
        VAL_RESET_HEADER(dest, REB_FRAME);
        dest->payload.any_context.varlist = types_varlist; // canon FRAME!
        dest->payload.any_context.phase = FUN(paramlist);
        INIT_BINDING(dest, UNBOUND);

        ++dest;

        REBVAL *src = DS_AT(dsp_orig + 2);
        src += 3;
        for (; src <= DS_TOP; src += 3) {
            assert(IS_BLOCK(src));
            if (definitional_return && src == definitional_return + 1)
                continue;

            if (VAL_ARRAY_LEN_AT(src) == 0)
                Init_Void(dest);
            else
                Move_Value(dest, src);
            ++dest;
        }

        if (definitional_return) {
            //
            // We put the return note in the top-level meta information, not
            // on the local itself (the "return-ness" is a distinct property
            // of the function from what word is used for RETURN:, and it
            // is possible to use the word RETURN for a local or refinement
            // argument while having nothing to do with the exit value of
            // the function.)
            //
            if (VAL_ARRAY_LEN_AT(definitional_return + 1) == 0)
                Init_Void(CTX_VAR(meta, STD_FUNCTION_META_RETURN_TYPE));
            else {
                Move_Value(
                    CTX_VAR(meta, STD_FUNCTION_META_RETURN_TYPE),
                    &definitional_return[1]
                );
            }

            if (NOT(flags & MKF_FAKE_RETURN)) {
                Init_Void(dest); // clear the local RETURN: var's description
                ++dest;
            }
        }

        TERM_ARRAY_LEN(types_varlist, num_slots);
        MANAGE_ARRAY(types_varlist);

        Init_Any_Context(
            CTX_VAR(meta, STD_FUNCTION_META_PARAMETER_TYPES),
            REB_FRAME,
            CTX(types_varlist)
        );
    }

    // Only make `parameter-notes` if there were strings (besides description)
    //
    if (NOT(has_notes)) {
        if (meta) {
            Init_Void(CTX_VAR(meta, STD_FUNCTION_META_PARAMETER_NOTES));
            Init_Void(CTX_VAR(meta, STD_FUNCTION_META_RETURN_NOTE));
        }
    }
    else {
        REBARR *notes_varlist = Make_Array_Core(
            num_slots, ARRAY_FLAG_VARLIST
        );
        MISC(notes_varlist).meta = NULL; // GC sees this, must initialize
        INIT_CTX_KEYLIST_SHARED(CTX(notes_varlist), paramlist);

        REBVAL *dest = SINK(ARR_HEAD(notes_varlist)); // "rootvar"
        VAL_RESET_HEADER(dest, REB_FRAME);
        dest->payload.any_context.varlist = notes_varlist; // canon FRAME!
        dest->payload.any_context.phase = FUN(paramlist);
        INIT_BINDING(dest, UNBOUND);

        ++dest;

        REBVAL *src = DS_AT(dsp_orig + 3);
        src += 3;
        for (; src <= DS_TOP; src += 3) {
            assert(IS_STRING(src));
            if (definitional_return && src == definitional_return + 2)
                continue;

            if (SER_LEN(VAL_SERIES(src)) == 0)
                Init_Void(dest);
            else
                Move_Value(dest, src);
            ++dest;
        }

        if (definitional_return) {
            //
            // See remarks on the return type--the RETURN is documented in
            // the top-level META-OF, not the "incidentally" named RETURN
            // parameter in the list
            //
            if (SER_LEN(VAL_SERIES(definitional_return + 2)) == 0)
                Init_Void(CTX_VAR(meta, STD_FUNCTION_META_RETURN_NOTE));
            else {
                Move_Value(
                    CTX_VAR(meta, STD_FUNCTION_META_RETURN_NOTE),
                    &definitional_return[2]
                );
            }

            if (NOT(flags & MKF_FAKE_RETURN)) {
                Init_Void(dest);
                ++dest;
            }
        }

        TERM_ARRAY_LEN(notes_varlist, num_slots);
        MANAGE_ARRAY(notes_varlist);

        Init_Any_Context(
            CTX_VAR(meta, STD_FUNCTION_META_PARAMETER_NOTES),
            REB_FRAME,
            CTX(notes_varlist)
        );
    }

    // With all the values extracted from stack to array, restore stack pointer
    //
    DS_DROP_TO(dsp_orig);

    return paramlist;
}



//
//  Find_Param_Index: C
//
// Find function param word in function "frame".
//
// !!! This is semi-redundant with similar functions for Find_Word_In_Array
// and key finding for objects, review...
//
REBCNT Find_Param_Index(REBARR *paramlist, REBSTR *spelling)
{
    REBSTR *canon = STR_CANON(spelling); // don't recalculate each time

    RELVAL *param = ARR_AT(paramlist, 1);
    REBCNT len = ARR_LEN(paramlist);

    REBCNT n;
    for (n = 1; n < len; ++n, ++param) {
        if (
            spelling == VAL_PARAM_SPELLING(param)
            || canon == VAL_PARAM_CANON(param)
        ) {
            return n;
        }
    }

    return 0;
}


//
//  Make_Function: C
//
// Create an archetypal form of a function, given C code implementing a
// dispatcher that will be called by Do_Core.  Dispatchers are of the form:
//
//     REB_R Dispatcher(REBFRM *f) {...}
//
// The REBFUN returned is "archetypal" because individual REBVALs which hold
// the same REBFUN may differ in a per-REBVAL piece of "instance" data.
// (This is how one RETURN is distinguished from another--the instance
// data stored in the REBVAL identifies the pointer of the FRAME! to exit).
//
// Functions have an associated REBVAL-sized cell of data, accessible via
// FUNC_BODY().  This is where they can store information that will be
// available when the dispatcher is called.  Despite being called "body", it
// doesn't have to be an array--it can be any REBVAL.
//
REBFUN *Make_Function(
    REBARR *paramlist,
    REBNAT dispatcher, // native C function called by Do_Core
    REBARR *opt_facade, // if provided, 0 element must be underlying function
    REBCTX *opt_exemplar // if provided, should be consistent w/next level
){
    ASSERT_ARRAY_MANAGED(paramlist);

    RELVAL *rootparam = ARR_HEAD(paramlist);
    assert(IS_FUNCTION(rootparam)); // !!! body not fully formed...
    assert(rootparam->payload.function.paramlist == paramlist);
    assert(VAL_BINDING(rootparam) == UNBOUND); // archetype

    // Precalculate cached function flags.
    //
    // Note: FUNC_FLAG_DEFERS_LOOKBACK is only relevant for un-refined-calls.
    // No lookback function calls trigger from PATH!.  HOWEVER: specialization
    // does come into play because it may change what the first "real"
    // argument is.  But again, we're only interested in specialization's
    // removal of *non-refinement* arguments.

    REBOOL first_arg = TRUE;

    REBVAL *param = KNOWN(rootparam) + 1;
    for (; NOT_END(param); ++param) {
        switch (VAL_PARAM_CLASS(param)) {
        case PARAM_CLASS_LOCAL:
            break; // skip

        case PARAM_CLASS_RETURN: {
            assert(VAL_PARAM_SYM(param) == SYM_RETURN);

            // See notes on FUNC_FLAG_INVISIBLE.
            //
            if (VAL_TYPESET_BITS(param) == 0)
                SET_VAL_FLAG(rootparam, FUNC_FLAG_INVISIBLE);
            break; }

        case PARAM_CLASS_LEAVE: {
            assert(VAL_PARAM_SYM(param) == SYM_LEAVE);
            break; } // skip.

        case PARAM_CLASS_REFINEMENT:
            //
            // hit before hitting any basic args, so not a brancher, and not
            // a candidate for deferring lookback arguments.
            //
            first_arg = FALSE;
            break;

        case PARAM_CLASS_NORMAL:
            //
            // First argument is not tight, and not specialized, so cache flag
            // to report that fact.
            //
            if (first_arg && NOT_VAL_FLAG(param, TYPESET_FLAG_HIDDEN)) {
                SET_VAL_FLAG(rootparam, FUNC_FLAG_DEFERS_LOOKBACK);
                first_arg = FALSE;
            }
            break;

        // Otherwise, at least one argument but not one that requires the
        // deferring of lookback.

        case PARAM_CLASS_TIGHT:
            //
            // If first argument is tight, and not specialized, no flag needed
            //
            if (first_arg && NOT_VAL_FLAG(param, TYPESET_FLAG_HIDDEN))
                first_arg = FALSE;
            break;

        case PARAM_CLASS_HARD_QUOTE:
        case PARAM_CLASS_SOFT_QUOTE:
            if (first_arg && NOT_VAL_FLAG(param, TYPESET_FLAG_HIDDEN)) {
                SET_VAL_FLAG(rootparam, FUNC_FLAG_QUOTES_FIRST_ARG);
                first_arg = FALSE;
            }
            break;

        default:
            assert(FALSE);
        }
    }

    // The "body" for a function can be any REBVAL.  It doesn't have to be
    // a block--it's anything that the dispatcher might wish to interpret.

    REBARR *body_holder = Alloc_Singular_Array();
    Init_Blank(ARR_SINGLE(body_holder));
    MANAGE_ARRAY(body_holder);

    rootparam->payload.function.body_holder = body_holder;

    // The C function pointer is stored inside the REBSER node for the body.
    // Hence there's no need for a `switch` on a function class in Do_Core,
    // Having a level of indirection from the REBVAL bits themself also
    // facilitates the "Hijacker" to change multiple REBVALs behavior.

    MISC(body_holder).dispatcher = dispatcher;

    // When this function is run, it needs to push a stack frame with a
    // certain number of arguments, and do type checking and parameter class
    // conventions based on that.  This frame must be compatible with the
    // number of arguments expected by the underlying function, and must not
    // allow any types to be passed to that underlying function it is not
    // expecting (e.g. natives written to only take INTEGER! may crash if
    // they get BLOCK!).  But beyond those constraints, the outer function
    // may have new parameter classes through a "facade".  This facade is
    // initially just the underlying function's paramlist, but may change.
    //
    if (opt_facade == NULL) {
        //
        // To avoid NULL checking when a function is called and looking for
        // the facade, just use the functions own paramlist if needed.  See
        // notes in Make_Paramlist_Managed_May_Fail() on why this has to be
        // pre-filled to avoid crashing on CTX_KEYLIST when making frames.
        //
        assert(LINK(paramlist).facade == paramlist);
    }
    else
        LINK(paramlist).facade = opt_facade;

    if (opt_exemplar == NULL) {
        //
        // !!! There may be some efficiency hack where this could be END, so
        // that when a REBFRM's ->special field is set there's no need to
        // check for NULL.
        //
        LINK(body_holder).exemplar = NULL;
    }
    else {
        // Because a dispatcher can update the phase and swap in the next
        // function with R_REDO_XXX, consistency checking isn't easily
        // done on whether the exemplar is "compatible" (and there may be
        // dispatcher forms which intentionally muck with the exemplar to
        // be incompatible, but these don't exist yet.)  So just check it's
        // compatible with the underlying frame.
        //
        // Base it off the facade since FUNC_NUM_PARAMS(FUNC_UNDERLYING())
        // would assert, since the function we're making is incomplete..
        //
        assert(
            CTX_LEN(opt_exemplar) == ARR_LEN(LINK(paramlist).facade) - 1
        );

        LINK(body_holder).exemplar = opt_exemplar;
    }

    // The meta information may already be initialized, since the native
    // version of paramlist construction sets up the FUNCTION-META information
    // used by HELP.  If so, it must be a valid REBCTX*.  Otherwise NULL.
    //
    assert(
        MISC(paramlist).meta == NULL
        || GET_SER_FLAG(CTX_VARLIST(MISC(paramlist).meta), ARRAY_FLAG_VARLIST)
    );

    // Note: used to set the keys of natives as read-only so that the debugger
    // couldn't manipulate the values in a native frame out from under it,
    // potentially crashing C code (vs. just causing userspace code to
    // error).  That protection is now done to the frame series on reification
    // in order to be able to MAKE FRAME! and reuse the native's paramlist.

    assert(NOT_SER_FLAG(paramlist, ARRAY_FLAG_FILE_LINE));
    assert(NOT_SER_FLAG(body_holder, ARRAY_FLAG_FILE_LINE));

    return FUN(paramlist);
}


//
//  Make_Expired_Frame_Ctx_Managed: C
//
// Function bodies contain relative words and relative arrays.  Arrays from
// this relativized body may only be put into a specified REBVAL once they
// have been combined with a frame.
//
// Reflection asks for function body data, when no instance is called.  Hence
// a REBVAL must be produced somehow.  If the body is being copied, then the
// option exists to convert all the references to unbound...but this isn't
// representative of the actual connections in the body.
//
// There could be an additional "archetype" state for the relative binding
// machinery.  But making a one-off expired frame is an inexpensive option,
// at least while the specific binding is coming online.
//
// !!! To be written...was started for MOLD of function, and realized it's
// really only needed for the BODY-OF reflector that gives back REBVAL*
//
REBCTX *Make_Expired_Frame_Ctx_Managed(REBFUN *func)
{
    REBARR *varlist = Alloc_Singular_Array_Core(
        ARRAY_FLAG_VARLIST | CONTEXT_FLAG_STACK
    );
    MISC(varlist).meta = NULL; // seen by GC, must be initialized
    MANAGE_ARRAY(varlist);

    RELVAL *rootvar = ARR_SINGLE(varlist);
    VAL_RESET_HEADER(rootvar, REB_FRAME);
    rootvar->payload.any_context.varlist = varlist;
    rootvar->payload.any_context.phase = func;
    INIT_BINDING(rootvar, UNBOUND); // !!! is a binding relevant?

    // func stored by the link field of a REB_FRAME context's varlist which
    // indicates that the frame has finished running.  If it is stack-based,
    // then that also means the data values are unavailable.
    //
    REBCTX *expired = CTX(varlist);
    SET_SER_INFO(varlist, SERIES_INFO_INACCESSIBLE);
    INIT_CTX_KEYLIST_SHARED(expired, FUNC_PARAMLIST(func));

    return expired;
}


//
//  Get_Maybe_Fake_Func_Body: C
//
// The FUNC_FLAG_LEAVE and FUNC_FLAG_RETURN tricks used for definitional
// scoping make it seem like a generator authored more code in the function's
// body...but the code isn't *actually* there and an optimized internal
// trick is used.
//
// If the body is fake, it needs to be freed by the caller with
// Free_Series.  This means that the body must currently be shallow
// copied, and the splicing slot must be in the topmost series.
//
REBARR *Get_Maybe_Fake_Func_Body(REBOOL *is_fake, const REBVAL *func)
{
    REBARR *fake_body;
    REBVAL *example = NULL;

    assert(IS_FUNCTION(func) && IS_FUNCTION_INTERPRETED(func));

    REBCNT body_index;
    if (GET_VAL_FLAG(func, FUNC_FLAG_RETURN)) {
        if (GET_VAL_FLAG(func, FUNC_FLAG_LEAVE)) {
            assert(FALSE); // !!! none of these actually exist
            example = Get_System(SYS_STANDARD, STD_FUNC_WITH_LEAVE_BODY);
            body_index = 8;
        }
        else {
            example = Get_System(SYS_STANDARD, STD_FUNC_BODY);
            body_index = 4;
        }
        *is_fake = TRUE;
    }
    else if (GET_VAL_FLAG(func, FUNC_FLAG_LEAVE)) {
        example = Get_System(SYS_STANDARD, STD_PROC_BODY);
        body_index = 4;
        *is_fake = TRUE;
    }
    else {
        *is_fake = FALSE;
        return VAL_ARRAY(VAL_FUNC_BODY(func));
    }

    // See comments in sysobj.r on standard/func-body and standard/proc-body
    //
    fake_body = Copy_Array_Shallow(VAL_ARRAY(example), VAL_SPECIFIER(example));

    // Index 5 (or 4 in zero-based C) should be #BODY, a "real" body.  Since
    // the body has relative words and relative arrays and this is not pairing
    // that with a frame from any specific invocation, the value must be
    // marked as relative.
    {
        RELVAL *slot = ARR_AT(fake_body, body_index); // #BODY
        assert(IS_ISSUE(slot));

        VAL_RESET_HEADER_EXTRA(slot, REB_GROUP, 0); // clear VAL_FLAG_LINE
        INIT_VAL_ARRAY(slot, VAL_ARRAY(VAL_FUNC_BODY(func)));
        VAL_INDEX(slot) = 0;
        INIT_BINDING(slot, VAL_FUNC(func)); // relative binding
    }

    return fake_body;
}


//
//  Make_Interpreted_Function_May_Fail: C
//
// This is the support routine behind `MAKE FUNCTION!`, FUNC, and PROC.
//
// Ren/C's schematic for the FUNC and PROC generators is *very* different
// from R3-Alpha, whose definition of FUNC was simply:
//
//     make function! copy/deep reduce [spec body]
//
// Ren/C's `make function!` doesn't need to copy the spec (it does not save
// it--parameter descriptions are in a meta object).  It also copies the body
// by virtue of the need to relativize it.  They also have "definitional
// return" constructs so that the body introduces RETURN and LEAVE constructs
// specific to each function invocation, so the body acts more like:
//
//     return: make function! [
//         [{Returns a value from a function.} value [<opt> any-value!]]
//         [unwind/with (context of 'return) :value]
//     ]
//     (body goes here)
//
// This pattern addresses "Definitional Return" in a way that does not
// technically require building RETURN or LEAVE in as a language keyword in
// any specific form (in the sense that MAKE FUNCTION! does not itself
// require it, and one can pretend FUNC and PROC don't exist).
//
// FUNC and PROC optimize by not internally building or executing the
// equivalent body, but giving it back from BODY-OF.  This is another benefit
// of making a copy--since the user cannot access the new root, it makes it
// possible to "lie" about what the body "above" is.  This gives FUNC and PROC
// the edge to pretend to add containing code and simulate its effects, while
// really only holding onto the body the caller provided.
//
// While plain MAKE FUNCTION! has no RETURN, UNWIND can be used to exit frames
// but must be explicit about what frame is being exited.  This can be used
// by usermode generators that want to create something return-like.
//
REBFUN *Make_Interpreted_Function_May_Fail(
    const REBVAL *spec,
    const REBVAL *code,
    REBFLGS mkf_flags // MKF_RETURN, MKF_LEAVE, etc.
) {
    assert(IS_BLOCK(spec));
    assert(IS_BLOCK(code));

    REBFUN *fun = Make_Function(
        Make_Paramlist_Managed_May_Fail(spec, mkf_flags),
        &Noop_Dispatcher, // will be overwritten if non-NULL body
        NULL, // no facade (use paramlist)
        NULL // no specialization exemplar (or inherited exemplar)
    );

    // We look at the *actual* function flags; e.g. the person may have used
    // the FUNC generator (with MKF_RETURN) but then named a parameter RETURN
    // which overrides it, so the value won't have FUNC_FLAG_RETURN.
    //
    REBVAL *value = FUNC_VALUE(fun);

    REBARR *body_array;
    if (VAL_ARRAY_LEN_AT(code) == 0) {
        if (GET_VAL_FLAG(value, FUNC_FLAG_INVISIBLE))
            FUNC_DISPATCHER(fun) = &Commenter_Dispatcher;
        else if (GET_VAL_FLAG(value, FUNC_FLAG_RETURN)) {
            //
            // Since we're bypassing type checking in the dispatcher for
            // speed, we need to make sure that the return type allows void
            // (which is all the Noop dispatcher will return).  If not, we
            // don't want to fail here (it would reveal the optimization)...
            // just fall back on the Returner_Dispatcher instead.
            //
            REBVAL *typeset = FUNC_PARAM(fun, FUNC_NUM_PARAMS(fun));
            assert(VAL_PARAM_SYM(typeset) == SYM_RETURN);
            if (!TYPE_CHECK(typeset, REB_MAX_VOID))
                FUNC_DISPATCHER(fun) = &Returner_Dispatcher;
        }

        // We could reuse the EMPTY_ARRAY, however that would be a fairly
        // esoteric optimization...and also, it would not give us anywhere to
        // put the ARRAY_FLAG_FILE_LINE bits.
        //
        body_array = Make_Array_Core(1, NODE_FLAG_MANAGED);
    }
    else {
        // Body is not empty, so we need to pick the right dispatcher based
        // on how the output value is to be handled.
        //
        if (GET_VAL_FLAG(value, FUNC_FLAG_INVISIBLE))
            FUNC_DISPATCHER(fun) = &Elider_Dispatcher; // no f->out mutation
        else if (GET_VAL_FLAG(value, FUNC_FLAG_RETURN))
            FUNC_DISPATCHER(fun) = &Returner_Dispatcher; // type checks f->out
        else if (GET_VAL_FLAG(value, FUNC_FLAG_LEAVE))
            FUNC_DISPATCHER(fun) = &Voider_Dispatcher; // forces f->out void
        else
            FUNC_DISPATCHER(fun) = &Unchecked_Dispatcher; // unchecked f->out

        // We need to copy the body in order to relativize its references to
        // args and locals to refer to the parameter list.  Future work
        // might be able to "image" the bindings virtually, and not require
        // this to be copied if the input code is read-only.
        //
        body_array = Copy_And_Bind_Relative_Deep_Managed(
            code,
            FUNC_PARAMLIST(fun),
            TS_ANY_WORD
        );
    }

    // We need to do a raw initialization of this block RELVAL because it is
    // relative to a function.  (Init_Block assumes all specific values.)
    //
    RELVAL *body = FUNC_BODY(fun);
    VAL_RESET_HEADER(body, REB_BLOCK);
    INIT_VAL_ARRAY(body, body_array);
    VAL_INDEX(body) = 0;
    INIT_BINDING(body, fun); // relative binding

    // The body array series ->misc and ->link fields are used for function
    // specific features.  But if the array *content* of a body array is a
    // series then the ->misc and ->link can be used to get FILE OF or LINE OF
    // a FUNCTION!, as it is usermode.
    //
    // Favor the spec first, then the body.
    //
    if (GET_SER_FLAG(VAL_ARRAY(spec), ARRAY_FLAG_FILE_LINE)) {
        LINK(body_array).file = LINK(VAL_ARRAY(spec)).file;
        MISC(body_array).line = MISC(VAL_ARRAY(spec)).line;
        SET_SER_FLAG(body_array, ARRAY_FLAG_FILE_LINE);
    }
    else if (GET_SER_FLAG(VAL_ARRAY(code), ARRAY_FLAG_FILE_LINE)) {
        LINK(body_array).file = LINK(VAL_ARRAY(code)).file;
        MISC(body_array).line = MISC(VAL_ARRAY(code)).line;
        SET_SER_FLAG(body_array, ARRAY_FLAG_FILE_LINE);
    }
    else {
        // Ideally all source series should have a file and line numbering
        // At the moment, if a function is created in the body of another
        // function it doesn't work...trying to fix that.
    }

    // All the series inside of a function body are "relatively bound".  This
    // means that there's only one copy of the body, but the series handle
    // is "viewed" differently based on which call it represents.  Though
    // each of these views compares uniquely, there's only one series behind
    // it...hence the series must be read only to keep modifying a view
    // that seems to have one identity but then affecting another.
    //
#if defined(NDEBUG)
    Deep_Freeze_Array(VAL_ARRAY(body));
#else
    if (!LEGACY(OPTIONS_UNLOCKED_SOURCE))
        Deep_Freeze_Array(VAL_ARRAY(body));
#endif

    return fun;
}


//
//  Make_Frame_For_Function: C
//
// This creates a *non-stack-allocated* FRAME!, which can be used in function
// applications or specializations.  It reuses the keylist of the function
// but makes a new varlist.
//
void Make_Frame_For_Function(
    REBVAL *out,
    const REBVAL *value // need the binding, can't just be a REBFUN*
){
    REBFUN *func = VAL_FUNC(value);
    REBCTX *exemplar = FUNC_EXEMPLAR(func); // may be NULL

    REBCNT facade_len = FUNC_FACADE_NUM_PARAMS(func) + 1;
    REBARR *varlist = Make_Array_Core(
        facade_len, // +1 for the CTX_VALUE() at [0]
        ARRAY_FLAG_VARLIST | SERIES_FLAG_FIXED_SIZE
    );

    REBVAL *rootvar = SINK(ARR_HEAD(varlist));
    VAL_RESET_HEADER(rootvar, REB_FRAME);
    rootvar->payload.any_context.varlist = varlist;
    rootvar->payload.any_context.phase = func;
    INIT_BINDING(rootvar, VAL_BINDING(value));

    REBVAL *arg = rootvar + 1;
    REBVAL *param = FUNC_FACADE_HEAD(func);

    if (exemplar == NULL) {
        //
        // No prior specialization means all the slots should be void.
        //
        for (; NOT_END(param); ++param, ++arg)
            Init_Void(arg);
    }
    else {
        // Partially specialized refinements put INTEGER! in refinement slots
        // (see notes on REB_0_PARTIAL for the mechanic).  But we don't want
        // to leak that to the user.  Convert to TRUE or void as appropriate,
        // so FRAME! won't show these refinements.
        //
        // !!! This loses the ordering, see Make_Frame_For_Specialization for
        // a frame-making mechanic which preserves it.
        //
        // !!! Logic is duplicated in Apply_Def_Or_Exemplar with the slight
        // change of needing to prep stack cells; review.
        //
        REBVAL *special = CTX_VARS_HEAD(exemplar);
        for (; NOT_END(param); ++param, ++arg, ++special) {
            if (VAL_PARAM_CLASS(param) != PARAM_CLASS_REFINEMENT) {
                Move_Value(arg, special);
                continue;
            }
            if (IS_LOGIC(special)) { // fully specialized, or disabled
                Init_Logic(arg, VAL_LOGIC(special));
                continue;
            }

            // See %c-special.c for an overview of why a REFINEMENT! in an
            // exemplar slot and void have a complex interpretation.
            //
            // Drive whether the refinement is present or not based on whether
            // it's available for the user to pass in or not.
            //
            assert(IS_REFINEMENT(special) || IS_VOID(special));
            if (IS_REFINEMENT_SPECIALIZED(param))
                Init_Logic(arg, TRUE);
            else
                Init_Void(arg);
        }
    }

    TERM_ARRAY_LEN(varlist, facade_len);

    MISC(varlist).meta = NULL; // GC sees this, we must initialize

    // The facade of the function is used as the keylist of the frame, as
    // that is how many values the frame must ultimately have.  Since this
    // is not a stack frame, there will be no ->phase to override it...the
    // FRAME! will always be viewed with those keys.
    //
    // Also, for things like definitional RETURN and LEAVE we had to stow the
    // `binding` field in the FRAME! REBVAL, since the single archetype
    // paramlist does not hold enough information to know where to return to.
    //
    // Note that this precludes the LINK().keysource from holding a REBFRM*,
    // since it is holding a parameter list instead.
    //
    INIT_CTX_KEYLIST_SHARED(CTX(varlist), FUNC_FACADE(func));
    ASSERT_ARRAY_MANAGED(CTX_KEYLIST(CTX(varlist)));

    Init_Any_Context(out, REB_FRAME, CTX(varlist));
    out->payload.any_context.phase = func;
}


//
//  REBTYPE: C
//
// This handler is used to fail for a type which cannot handle actions.
//
// !!! Currently all types have a REBTYPE() handler for either themselves or
// their class.  But having a handler that could be "swapped in" from a
// default failing case is an idea that could be used as an interim step
// to allow something like REB_GOB to fail by default, but have the failing
// type handler swapped out by an extension.
//
REBTYPE(Fail)
{
    UNUSED(frame_);
    UNUSED(action);

    fail ("Datatype does not have a dispatcher registered.");
}


//
//  Action_Dispatcher: C
//
// "actions" are historically a kind of dispatch based on the first argument's
// type, and then calling a common function for that type parameterized with
// a word for the action.  e.g. APPEND X [...] would look at the type of X,
// and call a function based on that parameterized with APPEND and the list
// of arguments.
//
REB_R Action_Dispatcher(REBFRM *f)
{
    enum Reb_Kind kind = VAL_TYPE(FRM_ARG(f, 1));
    REBSYM sym = VAL_WORD_SYM(FUNC_BODY(f->phase));
    assert(sym != SYM_0);

    // !!! Some reflectors are more general and apply to all types (e.g. TYPE)
    // while others only apply to some types (e.g. LENGTH or HEAD only to
    // series, or perhaps things like PORT! that wish to act like a series).
    // This suggests a need for a kind of hierarchy of handling.
    //
    // The series common code is in Series_Common_Action_Maybe_Unhandled(),
    // but that is only called from series.  Handle a few extra cases here.
    //
    if (sym == SYM_REFLECT) {
        REBFRM *frame_ = f;
        INCLUDE_PARAMS_OF_REFLECT;

        UNUSED(ARG(value));
        REBSYM property = VAL_WORD_SYM(ARG(property));

        switch (property) {
        case SYM_0:
            //
            // If a word wasn't in %words.r, it has no integer SYM.  There is
            // no way for a built-in reflector to handle it...since they just
            // operate on SYMs in a switch().  Longer term, a more extensible
            // idea may be necessary.
            //
            fail (Error_Cannot_Reflect(kind, ARG(property)));

        case SYM_TYPE:
            if (kind == REB_MAX_VOID)
                return R_BLANK;
            Val_Init_Datatype(f->out, kind);
            return R_OUT;

        default:
            // !!! Are there any other universal reflectors?
            break;
        }
    }

    // !!! The reflector for TYPE is universal and so it is allowed on voids,
    // but in general actions should not allow void first arguments...there's
    // no entry in the dispatcher table for them.
    //
    if (kind == REB_MAX_VOID)
        fail ("VOID isn't valid for REFLECT, except for TYPE OF ()");

    assert(kind < REB_MAX);

    REBACT subdispatch = Value_Dispatch[kind];
    return subdispatch(f, sym);
}


//
//  Noop_Dispatcher: C
//
// If a function's body is an empty block, rather than bother running the
// equivalent of `DO []` and generating a frame for specific binding, this
// just returns void.  What makes this a semi-interesting optimization is
// for functions like ASSERT whose default implementation is an empty block,
// but intended to be hijacked in "debug mode" with an implementation.  So
// you can minimize the cost of instrumentation hooks.
//
REB_R Noop_Dispatcher(REBFRM *f)
{
    assert(VAL_LEN_AT(FUNC_BODY(f->phase)) == 0);
    UNUSED(f);
    return R_VOID;
}


//
//  Datatype_Checker_Dispatcher: C
//
// Dispatcher used by TYPECHECKER generator for when argument is a datatype.
//
REB_R Datatype_Checker_Dispatcher(REBFRM *f)
{
    RELVAL *datatype = FUNC_BODY(f->phase);
    assert(IS_DATATYPE(datatype));
    if (VAL_TYPE(FRM_ARG(f, 1)) == VAL_TYPE_KIND(datatype))
        return R_TRUE;
    return R_FALSE;
}


//
//  Typeset_Checker_Dispatcher: C
//
// Dispatcher used by TYPECHECKER generator for when argument is a typeset.
//
REB_R Typeset_Checker_Dispatcher(REBFRM *f)
{
    RELVAL *typeset = FUNC_BODY(f->phase);
    assert(IS_TYPESET(typeset));
    if (TYPE_CHECK(typeset, VAL_TYPE(FRM_ARG(f, 1))))
        return R_TRUE;
    return R_FALSE;
}


//
//  Unchecked_Dispatcher: C
//
// This is the default MAKE FUNCTION! dispatcher for interpreted functions
// (whose body is a block that runs through DO []).  There is no return type
// checking done on these simple functions.
//
REB_R Unchecked_Dispatcher(REBFRM *f)
{
    RELVAL *body = FUNC_BODY(f->phase);
    assert(IS_BLOCK(body) && IS_RELATIVE(body) && VAL_INDEX(body) == 0);

    if (Do_At_Throws(f->out, VAL_ARRAY(body), 0, SPC(f)))
        return R_OUT_IS_THROWN;

    return R_OUT;
}


//
//  Voider_Dispatcher: C
//
// Variant of Unchecked_Dispatcher, except sets the output value to void.
// Pushing that code into the dispatcher means there's no need to do flag
// testing in the main loop.
//
REB_R Voider_Dispatcher(REBFRM *f)
{
    RELVAL *body = FUNC_BODY(f->phase);
    assert(IS_BLOCK(body) && IS_RELATIVE(body) && VAL_INDEX(body) == 0);

    if (Do_At_Throws(f->out, VAL_ARRAY(body), 0, SPC(f)))
        return R_OUT_IS_THROWN;

    return R_VOID;
}


//
//  Returner_Dispatcher: C
//
// Contrasts with the Unchecked_Dispatcher since it ensures the return type is
// correct.  (Note that natives do not get this type checking, and they
// probably shouldn't pay for it except in the debug build.)
//
REB_R Returner_Dispatcher(REBFRM *f)
{
    RELVAL *body = FUNC_BODY(f->phase);
    assert(IS_BLOCK(body) && IS_RELATIVE(body) && VAL_INDEX(body) == 0);

    if (Do_At_Throws(f->out, VAL_ARRAY(body), 0, SPC(f)))
        return R_OUT_IS_THROWN;

    REBVAL *typeset = FUNC_PARAM(f->phase, FUNC_NUM_PARAMS(f->phase));
    assert(VAL_PARAM_SYM(typeset) == SYM_RETURN);

    // Typeset bits for locals in frames are usually ignored, but the RETURN:
    // local uses them for the return types of a "virtual" definitional return
    // if the parameter is PARAM_CLASS_RETURN.
    //
    if (!TYPE_CHECK(typeset, VAL_TYPE(f->out)))
        fail (Error_Bad_Return_Type(f, VAL_TYPE(f->out)));

    return R_OUT;
}


//
//  Elider_Dispatcher: C
//
// This is used by "invisible" functions (who in their spec say `return: []`).
// The goal is to evaluate a function call in such a way that its presence
// doesn't disrupt the chain of evaluation any more than if the call were not
// there.  (The call can have side effects, however.)
//
REB_R Elider_Dispatcher(REBFRM *f)
{
    RELVAL *body = FUNC_BODY(f->phase);
    assert(IS_BLOCK(body) && IS_RELATIVE(body) && VAL_INDEX(body) == 0);

    // !!! It would be nice to use the frame's spare "cell" for the thrownaway
    // result, but Fetch_Next code expects to use the cell.
    //
    DECLARE_LOCAL (dummy);
    SET_END(dummy);

    if (Do_At_Throws(dummy, VAL_ARRAY(body), 0, SPC(f))) {
        Move_Value(f->out, dummy);
        return R_OUT_IS_THROWN;
    }

    return R_INVISIBLE;
}


//
//  Commenter_Dispatcher: C
//
// This is a specialized version of Elider_Dispatcher() for when the body of
// a function is empty.  This helps COMMENT and functions like it run faster.
//
REB_R Commenter_Dispatcher(REBFRM *f)
{
    assert(VAL_LEN_AT(FUNC_BODY(f->phase)) == 0);
    UNUSED(f);
    return R_INVISIBLE;
}


//
//  Hijacker_Dispatcher: C
//
// A hijacker takes over another function's identity, replacing it with its
// own implementation, injecting directly into the paramlist and body_holder
// nodes held onto by all the victim's references.
//
// Sometimes the hijacking function has the same underlying function
// as the victim, in which case there's no need to insert a new dispatcher.
// The hijacker just takes over the identity.  But otherwise it cannot,
// and a "shim" is needed...since something like an ADAPT or SPECIALIZE
// or a MAKE FRAME! might depend on the existing paramlist shape.
//
REB_R Hijacker_Dispatcher(REBFRM *f)
{
    RELVAL *hijacker = FUNC_BODY(f->phase);

    // We need to build a new frame compatible with the hijacker, and
    // transform the parameters we've gathered to be compatible with it.
    //
    if (Redo_Func_Throws(f, VAL_FUNC(hijacker)))
        return R_OUT_IS_THROWN;

    return R_OUT;
}


//
//  Adapter_Dispatcher: C
//
// Dispatcher used by ADAPT.
//
REB_R Adapter_Dispatcher(REBFRM *f)
{
    RELVAL *adaptation = FUNC_BODY(f->phase);
    assert(ARR_LEN(VAL_ARRAY(adaptation)) == 2);

    RELVAL* prelude = VAL_ARRAY_AT_HEAD(adaptation, 0);
    REBVAL* adaptee = KNOWN(VAL_ARRAY_AT_HEAD(adaptation, 1));

    // The first thing to do is run the prelude code, which may throw.  If it
    // does throw--including a RETURN--that means the adapted function will
    // not be run.
    //
    // (Note that when the adapter was created, the prelude code was bound to
    // the paramlist of the *underlying* function--because that's what a
    // compatible frame gets pushed for.)
    //
    if (Do_At_Throws(f->out, VAL_ARRAY(prelude), VAL_INDEX(prelude), SPC(f)))
        return R_OUT_IS_THROWN;

    f->phase = VAL_FUNC(adaptee);
    f->binding = VAL_BINDING(adaptee);
    return R_REDO_CHECKED; // Have Do_Core run the adaptee updated into f->phase
}


//
//  Encloser_Dispatcher: C
//
// Dispatcher used by ENCLOSE.
//
REB_R Encloser_Dispatcher(REBFRM *f)
{
    RELVAL *enclosure = FUNC_BODY(f->phase);
    assert(ARR_LEN(VAL_ARRAY(enclosure)) == 2);

    RELVAL* inner = KNOWN(VAL_ARRAY_AT_HEAD(enclosure, 0)); // same args as f
    assert(IS_FUNCTION(inner));
    REBVAL* outer = KNOWN(VAL_ARRAY_AT_HEAD(enclosure, 1)); // 1 FRAME! arg
    assert(IS_FUNCTION(outer));

    // We want to call OUTER with a FRAME! value that will dispatch to INNER
    // when it runs DO on it.  The contents of the arguments for that call to
    // inner should start out as the same as what has been built for the
    // passed in F.  (OUTER may mutate these before the call if it likes.)
    //
    // !!! It is desirable in the general case to just reuse the values in
    // the chunk stack that f already has for inner.  However, inner is going
    // to be called at a deeper stack level than outer.  This tampers with
    // the logic of the system for things like Move_Value(), which have to
    // make decisions about the relative lifetimes of cells in order to
    // decide whether to reify things (like REBFRM* to a REBSER* for FRAME!)
    //
    // !!! To get the ball rolling with testing the feature, pass a copy of
    // the frame values in a heap-allocated FRAME!...which it will turn around
    // and stack allocate again when DO is called.  That's triply inefficient
    // because it forces reification of the stub frame just to copy it...
    // which is not necessary, but easier code to write since it can use
    // Copy_Context_Core().  Tune this all up as it becomes more mainstream,
    // since you don't need to make 1 copy of the values...much less 2.

    const REBU64 types = 0;
    REBCTX *copy = Copy_Context_Core(
        Context_For_Frame_May_Reify_Managed(f), types
    );

    DECLARE_LOCAL (arg);
    Init_Any_Context(arg, REB_FRAME, copy);

    // !!! Review how exactly this update to the phase and binding is supposed
    // to work.  We know that when `outer` tries to DO its frame argument,
    // it needs to run inner with the correct binding.
    //
    arg->payload.any_context.phase = VAL_FUNC(inner);
    INIT_BINDING(arg, VAL_BINDING(inner));

    const REBOOL fully = TRUE;
    if (Apply_Only_Throws(f->out, fully, outer, arg, END))
        return R_OUT_IS_THROWN;

    return R_OUT;
}


//
//  Chainer_Dispatcher: C
//
// Dispatcher used by CHAIN.
//
REB_R Chainer_Dispatcher(REBFRM *f)
{
    REBVAL *pipeline = KNOWN(FUNC_BODY(f->phase)); // array of functions

    // Before skipping off to find the underlying non-chained function
    // to kick off the execution, the post-processing pipeline has to
    // be "pushed" so it is not forgotten.  Go in reverse order so
    // the function to apply last is at the bottom of the stack.
    //
    REBVAL *value = KNOWN(ARR_LAST(VAL_ARRAY(pipeline)));
    while (value != VAL_ARRAY_HEAD(pipeline)) {
        assert(IS_FUNCTION(value));
        DS_PUSH(KNOWN(value));
        --value;
    }

    // Extract the first function, itself which might be a chain.
    //
    f->phase = VAL_FUNC(value);
    f->binding = VAL_BINDING(value);

    return R_REDO_UNCHECKED; // signatures should match
}


//
//  Get_If_Word_Or_Path_Arg: C
//
// Some routines like APPLY and SPECIALIZE are willing to take a WORD! or
// PATH! instead of just the value type they are looking for, and perform
// the GET for you.  By doing the GET inside the function, they are able
// to preserve the symbol:
//
//     >> apply 'append [value: 'c]
//     ** Script error: append is missing its series argument
//
void Get_If_Word_Or_Path_Arg(
    REBVAL *out,
    REBSTR **opt_name_out,
    const REBVAL *value
) {
    DECLARE_LOCAL (adjusted);
    Move_Value(adjusted, value);

    if (ANY_WORD(value)) {
        *opt_name_out = VAL_WORD_SPELLING(value);
        VAL_SET_TYPE_BITS(adjusted, REB_GET_WORD);
    }
    else if (ANY_PATH(value)) {
        //
        // In theory we could get a symbol here, assuming we only do non
        // evaluated GETs.  Not implemented at the moment.
        //
        *opt_name_out = NULL;
        VAL_SET_TYPE_BITS(adjusted, REB_GET_PATH);
    }
    else {
        *opt_name_out = NULL;
        Move_Value(out, value);
        return;
    }

    if (Eval_Value_Throws(out, adjusted)) {
        //
        // !!! GET_PATH should not evaluate GROUP!, and hence shouldn't be
        // able to throw.  TBD.
        //
        fail (Error_No_Catch_For_Throw(out));
    }
}


//
//  Apply_Def_Or_Exemplar: C
//
// Factors out common code used by DO of a FRAME!, and APPLY.
//
// !!! Because APPLY is being written as a regular native (and not a
// special exception case inside of Do_Core) it has to "re-enter" Do_Core
// and jump to the argument processing.  This is the first example of
// such a re-entry, and is not particularly streamlined yet.
//
// This could also be accomplished if function dispatch were a subroutine
// that would be called both here and from the evaluator loop.  But if
// the subroutine were parameterized with the frame state, it would be
// basically equivalent to a re-entry.  And re-entry is interesting to
// experiment with for other reasons (e.g. continuations), so that is what
// is used here.
//
REB_R Apply_Def_Or_Exemplar(
    REBVAL *out,
    REBFUN *fun,
    REBNOD *binding,
    REBSTR *opt_label,
    REBNOD *def_or_exemplar // REBVAL of a def block, or REBARR varlist
){
    DECLARE_FRAME (f);

    f->out = out;
    TRASH_POINTER_IF_DEBUG(f->gotten); // shouldn't be looked at (?)

    // We pretend our "input source" has ended.
    //
    f->source.index = 0;
    f->source.vaptr = NULL;
    f->source.array = EMPTY_ARRAY; // for setting HOLD flag in Push_Frame
    TRASH_POINTER_IF_DEBUG(f->source.pending);
    //
    f->gotten = END;
    SET_FRAME_VALUE(f, END);
    f->specifier = SPECIFIED;

    Init_Endlike_Header(&f->flags, DO_FLAG_APPLYING);

    Push_Frame_Core(f);

    Push_Function(f, opt_label, fun, binding);
    f->refine = ORDINARY_ARG;

    if (NOT_CELL(def_or_exemplar)) {
        //
        // When you DO a FRAME!, it feeds its varlist in to be copied into
        // the stack positions.
        //
        REBCTX *exemplar = CTX(def_or_exemplar);

        // Push_Function() defaults f->special to the exemplar of the function
        // but we wish to override it (with a maybe more filled frame)
        //
        f->special = CTX_VARS_HEAD(exemplar);
    }
    else {
        REBVAL *def = cast(REBVAL*, def_or_exemplar); // code that fills frame

        // For this one-off APPLY with a BLOCK!, we don't want to call
        // Make_Frame_For_Function() to get a heap object just for one use.
        // Better to DO the block directly into stack cells that will be used
        // in the function application.  But the code that fills the frame
        // can't see garbage, so go ahead and format the stack cells.
        //
        // !!! We will walk the parameters again to setup the binder; see
        // Make_Context_For_Specialization() for how loops could be combined.

        if (f->special == f->param) { // signals "no exemplar"
            for (; NOT_END(f->param); ++f->param, ++f->arg) {
                Prep_Stack_Cell(f->arg);
                Init_Void(f->arg);
            }
        }
        else {
            // !!! This needs more complex logic now with partial refinements;
            // code needs to be unified with Make_Frame_For_Function().  The
            // main difference is that this formats stack cells for direct use
            // vs. creating a heap object, but the logic is the same.

            for (; NOT_END(f->param); ++f->param, ++f->arg, ++f->special) {
                Prep_Stack_Cell(f->arg);
                if (VAL_PARAM_CLASS(f->param) != PARAM_CLASS_REFINEMENT) {
                    Move_Value(f->arg, f->special);
                    continue;
                }
                if (IS_LOGIC(f->special)) { // fully specialized, or disabled
                    Init_Logic(f->arg, VAL_LOGIC(f->special));
                    continue;
                }

                assert(IS_REFINEMENT(f->special) || IS_VOID(f->special));
                if (IS_REFINEMENT_SPECIALIZED(f->param))
                    Init_Logic(f->arg, TRUE);
                else
                    Init_Void(f->arg);
            }
            assert(IS_END(f->special));
        }

        assert(IS_END(f->arg)); // all other chunk stack cells unformatted

        // In today's implementation, the body must be rebound to the frame.
        // Ideally if it were read-only (at least), then the opt_def value
        // should be able to carry a virtual binding into the new context.
        // That feature is not currently implemented, so this mutates the
        // bindings on the passed in block...as OBJECTs and other things do
        //
        Bind_Values_Core(
            VAL_ARRAY_AT(def),
            Context_For_Frame_May_Reify_Managed(f),
            FLAGIT_KIND(REB_SET_WORD), // types to bind (just set-word!)
            0, // types to "add midstream" to binding as we go (nothing)
            BIND_DEEP
        );

        // Do the block into scratch cell, ignore the result (unless thrown)
        //
        if (Do_Any_Array_At_Throws(SINK(&f->cell), def)) {
            Drop_Frame_Core(f);
            Move_Value(f->out, KNOWN(&f->cell));
            return R_OUT_IS_THROWN;
        }

        f->arg = f->args_head; // reset
        f->param = FUNC_FACADE_HEAD(f->phase); // reset

        f->special = f->arg; // now signal only type-check the existing data
    }

    (*PG_Do)(f);

    Drop_Frame_Core(f);

    if (THROWN(f->out))
        return R_OUT_IS_THROWN; // prohibits recovery from exits

    assert(FRM_AT_END(f)); // we started at END_FLAG, can only throw

    return R_OUT;
}
