//
//  File: %t-money.c
//  Summary: "extended precision datatype"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2016 Rebol Open Source Contributors
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
#include "sys-deci-funcs.h"


//
//  CT_Money: C
//
REBINT CT_Money(const REBVAL *a, const REBVAL *b, REBINT mode)
{
    REBOOL e, g;

    e = deci_is_equal(VAL_MONEY_AMOUNT(a), VAL_MONEY_AMOUNT(b));
    if (mode < 0) {
        g = deci_is_lesser_or_equal(
            VAL_MONEY_AMOUNT(b), VAL_MONEY_AMOUNT(a)
        );
        if (mode == -1) e = LOGICAL(e || g);
        else e = LOGICAL(g && !e);
    }
    return e ? 1 : 0;
}


//
//  Emit_Money: C
//
REBINT Emit_Money(const REBVAL *value, REBYTE *buf, REBFLGS opts)
{
    return deci_to_string(buf, VAL_MONEY_AMOUNT(value), '$', '.');
}


//
//  Bin_To_Money_May_Fail: C
//
// Will successfully convert or fail (longjmp) with an error.
//
void Bin_To_Money_May_Fail(REBVAL *result, REBVAL *val)
{
    REBCNT len;
    REBYTE buf[MAX_HEX_LEN+4] = {0}; // binary to convert

    if (IS_BINARY(val)) {
        len = VAL_LEN_AT(val);
        if (len > 12) len = 12;
        memcpy(buf, VAL_BIN_AT(val), len);
    }
    else
        fail (Error_Invalid_Arg(val));

    memcpy(buf + 12 - len, buf, len); // shift to right side
    memset(buf, 0, 12 - len);
    VAL_MONEY_AMOUNT(result) = binary_to_deci(buf);
}


//
//  REBTYPE: C
//
REBTYPE(Money)
{
    REBVAL *val = D_ARG(1);
    REBVAL *arg;
    const REBYTE *str;
    REBINT equal = 1;

    if (IS_BINARY_ACT(action)) {
        arg = D_ARG(2);

        if (IS_MONEY(arg))
            ;
        else if (IS_INTEGER(arg)) {
            VAL_MONEY_AMOUNT(D_OUT) = int_to_deci(VAL_INT64(arg));
            arg = D_OUT;
        }
        else if (IS_DECIMAL(arg) || IS_PERCENT(arg)) {
            VAL_MONEY_AMOUNT(D_OUT) = decimal_to_deci(VAL_DECIMAL(arg));
            arg = D_OUT;
        }
        else
            fail (Error_Math_Args(REB_MONEY, action));

        switch (action) {
        case A_ADD:
            VAL_MONEY_AMOUNT(D_OUT) = deci_add(
                VAL_MONEY_AMOUNT(val), VAL_MONEY_AMOUNT(arg)
            );
            break;

        case A_SUBTRACT:
            VAL_MONEY_AMOUNT(D_OUT) = deci_subtract(
                VAL_MONEY_AMOUNT(val), VAL_MONEY_AMOUNT(arg)
            );
            break;

        case A_MULTIPLY:
            VAL_MONEY_AMOUNT(D_OUT) = deci_multiply(
                VAL_MONEY_AMOUNT(val), VAL_MONEY_AMOUNT(arg)
            );
            break;

        case A_DIVIDE:
            VAL_MONEY_AMOUNT(D_OUT) = deci_divide(
                VAL_MONEY_AMOUNT(val), VAL_MONEY_AMOUNT(arg)
            );
            break;

        case A_REMAINDER:
            VAL_MONEY_AMOUNT(D_OUT) = deci_mod(
                VAL_MONEY_AMOUNT(val), VAL_MONEY_AMOUNT(arg)
            );
            break;

        default:
            fail (Error_Illegal_Action(REB_MONEY, action));
        }

        VAL_RESET_HEADER(D_OUT, REB_MONEY);
        return R_OUT;
    }

    switch(action) {
    case A_NEGATE:
        VAL_MONEY_AMOUNT(val).s = !VAL_MONEY_AMOUNT(val).s;
        *D_OUT = *D_ARG(1);
        return R_OUT;

    case A_ABSOLUTE:
        VAL_MONEY_AMOUNT(val).s = 0;
        *D_OUT = *D_ARG(1);
        return R_OUT;

    case A_ROUND:
        arg = D_ARG(3);
        if (D_REF(2)) {
            if (IS_INTEGER(arg))
                VAL_MONEY_AMOUNT(arg) = int_to_deci(VAL_INT64(arg));
            else if (IS_DECIMAL(arg) || IS_PERCENT(arg))
                VAL_MONEY_AMOUNT(arg) = decimal_to_deci(VAL_DECIMAL(arg));
            else if (!IS_MONEY(arg)) fail (Error_Invalid_Arg(arg));
        }
        VAL_MONEY_AMOUNT(D_OUT) = Round_Deci(
            VAL_MONEY_AMOUNT(val),
            Get_Round_Flags(frame_),
            VAL_MONEY_AMOUNT(arg)
        );
        if (D_REF(2)) {
            if (IS_DECIMAL(arg) || IS_PERCENT(arg)) {
                REBDEC dec = deci_to_decimal(VAL_MONEY_AMOUNT(D_OUT));
                VAL_RESET_HEADER(D_OUT, VAL_TYPE(arg));
                VAL_DECIMAL(D_OUT) = dec;
                return R_OUT;
            }
            if (IS_INTEGER(arg)) {
                REBI64 i64 = deci_to_int(VAL_MONEY_AMOUNT(D_OUT));
                VAL_RESET_HEADER(D_OUT, REB_INTEGER);
                VAL_INT64(D_OUT) = i64;
                return R_OUT;
            }
        }
        break;

    case A_EVEN_Q:
    case A_ODD_Q:
        equal = 1 & (REBINT)deci_to_int(VAL_MONEY_AMOUNT(val));
        if (action == A_EVEN_Q) equal = !equal;
        if (equal) goto is_true;
        goto is_false;

    case A_MAKE:
    case A_TO:
        arg = D_ARG(2);

        switch (VAL_TYPE(arg)) {

        case REB_INTEGER:
            VAL_MONEY_AMOUNT(D_OUT) = int_to_deci(VAL_INT64(arg));
            break;

        case REB_DECIMAL:
        case REB_PERCENT:
            VAL_MONEY_AMOUNT(D_OUT) = decimal_to_deci(VAL_DECIMAL(arg));
            break;

        case REB_MONEY:
            *D_OUT = *D_ARG(2);
            return R_OUT;

        case REB_STRING:
        {
            const REBYTE *end;
            str = Temp_Byte_Chars_May_Fail(arg, MAX_SCAN_MONEY, 0, FALSE);
            VAL_MONEY_AMOUNT(D_OUT) = string_to_deci(str, &end);
            if (end == str || *end != 0) fail (Error_Bad_Make(REB_MONEY, arg));
            break;
        }

//      case REB_ISSUE:
        case REB_BINARY:
            Bin_To_Money_May_Fail(D_OUT, arg);
            break;

        case REB_LOGIC:
            equal = !VAL_LOGIC(arg);
//      case REB_NONE: // 'equal defaults to 1
            VAL_MONEY_AMOUNT(D_OUT) = int_to_deci(equal ? 0 : 1);
            break;

        default:
            fail (Error_Bad_Make(REB_MONEY, arg));
        }
        break;

    default:
        fail (Error_Illegal_Action(REB_MONEY, action));
    }

    VAL_RESET_HEADER(D_OUT, REB_MONEY);
    return R_OUT;

is_true:  return R_TRUE;
is_false: return R_FALSE;
}

