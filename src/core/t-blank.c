//
//  File: %t-blank.c
//  Summary: "Blank datatype"
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
//  MF_Bar: C
//
void MF_Bar(REB_MOLD *mo, const REBCEL *v, bool form)
{
    UNUSED(form); // no distinction between MOLD and FORM
    UNUSED(v);
    Append_Unencoded(mo->series, "|");
}


//
//  MF_Blank: C
//
void MF_Blank(REB_MOLD *mo, const REBCEL *v, bool form)
{
    UNUSED(form); // no distinction between MOLD and FORM
    UNUSED(v);
    Append_Unencoded(mo->series, "_");
}


//
//  MF_Void: C
//
// !!! No literal notation for VOID! values has been decided.
//
void MF_Void(REB_MOLD *mo, const REBCEL *v, bool form)
{
    UNUSED(form); // no distinction between MOLD and FORM
    UNUSED(v);
    Append_Unencoded(mo->series, "#[void]");
}


//
//  PD_Blank: C
//
// It is not possible to "poke" into a blank (and as an attempt at modifying
// operation, it is not swept under the rug).  But if picking with GET-PATH!
// or GET, we indicate no result with void.  (Ordinary path selection will
// treat this as an error.)
//
// This could also be taken care of with special code in path dispatch, but
// by putting it in a handler you only pay for the logic if you actually do
// encounter a blank.
//
REB_R PD_Blank(
    REBPVS *pvs,
    const REBVAL *picker,
    const REBVAL *opt_setval
){
    UNUSED(picker);
    UNUSED(pvs);

    if (opt_setval != NULL)
        return R_UNHANDLED;

    return nullptr;
}


//
//  MAKE_Unit: C
//
// MAKE is disallowed, with the general rule that a blank in will give
// a null out... for e.g. `make object! try select data spec else [...]`
//
REB_R MAKE_Unit(REBVAL *out, enum Reb_Kind kind, const REBVAL *arg) {
    UNUSED(out);
    fail (Error_Bad_Make(kind, arg));
}


//
//  TO_Unit: C
//
// TO is disallowed, e.g. you can't TO convert an integer of 0 to a blank.
//
REB_R TO_Unit(REBVAL *out, enum Reb_Kind kind, const REBVAL *data) {
    UNUSED(out);
    fail (Error_Bad_Make(kind, data));
}


//
//  CT_Unit: C
//
// Must have a comparison function, otherwise SORT would not work on arrays
// with blanks or voids in them.
//
REBINT CT_Unit(const REBCEL *a, const REBCEL *b, REBINT mode)
{
    if (mode >= 0)
        return (CELL_KIND(a) == CELL_KIND(b));
    return -1;
}


//
//  REBTYPE: C
//
// While generics like SELECT are able to dispatch on BLANK! and return NULL,
// they do so by not running at all...see REB_TS_NOOP_IF_BLANK.
//
// The only operations
//
REBTYPE(Unit)
{
    REBVAL *unit = D_ARG(1);

    switch (VAL_WORD_SYM(verb)) {
      case SYM_REFLECT: {
        INCLUDE_PARAMS_OF_REFLECT;
        UNUSED(ARG(value)); // taken care of by `unit` above.

        // !!! REFLECT cannot use REB_TS_NOOP_IF_BLANK, because of the special
        // case of TYPE OF...where a BLANK! in needs to provide BLANK! the
        // datatype out.  Also, there currently exist "reflectors" that
        // return LOGIC!, e.g. TAIL?...and logic cannot blindly return null:
        //
        // https://forum.rebol.info/t/954
        //
        // So for the moment, we just ad-hoc return nullptr for some that
        // R3-Alpha returned NONE! for.  Review.
        //
        switch (VAL_WORD_SYM(ARG(property))) {
          case SYM_INDEX:
          case SYM_LENGTH:
            return nullptr;

          default: break;
        }
        break; }

      case SYM_COPY: { // since `copy/deep [1 _ 2]` is legal, allow `copy _`
        INCLUDE_PARAMS_OF_COPY;
        UNUSED(ARG(value)); // already referenced as `unit`
        if (REF(part)) {
            UNUSED(ARG(limit));
            fail (Error_Bad_Refines_Raw());
        }
        UNUSED(REF(deep));
        UNUSED(REF(types));
        UNUSED(ARG(kinds));
        return Init_Blank(D_OUT); }

      default: break;
    }

    fail (Error_Illegal_Action(VAL_TYPE(unit), verb));
}



//
//  MF_Handle: C
//
void MF_Handle(REB_MOLD *mo, const REBCEL *v, bool form)
{
    // Value has no printable form, so just print its name.

    if (form)
        Emit(mo, "?T?", v);
    else
        Emit(mo, "+T", v);
}


//
//  CT_Handle: C
//
REBINT CT_Handle(const REBCEL *a, const REBCEL *b, REBINT mode)
{
    // Would it be meaningful to allow user code to compare HANDLE!?
    //
    UNUSED(a);
    UNUSED(b);
    UNUSED(mode);
    fail ("Currently comparing HANDLE! types is not allowed.");
}


//
// REBTYPE: C
//
// !!! Currently, in order to have a comparison function a datatype must also
// have a dispatcher for generics, and the comparison is essential.  Hence
// this cannot use a `-` in the %reb-types.r in lieu of this dummy function.
//
REBTYPE(Handle)
{
    UNUSED(frame_);

    fail (Error_Illegal_Action(REB_HANDLE, verb));
}
