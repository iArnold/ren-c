//
//  File: %p-dns.c
//  Summary: "DNS port interface"
//  Section: ports
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
#include "reb-net.h"


//
//  DNS_Actor: C
//
static REB_R DNS_Actor(REBFRM *frame_, REBVAL *port, REBVAL *verb)
{
    REBVAL *arg = D_ARGC > 1 ? D_ARG(2) : NULL;

    REBREQ *sock = Ensure_Port_State(port, RDI_DNS);
    sock->timeout = 4000; // where does this go? !!!

    REBCTX *ctx = VAL_CONTEXT(port);
    REBVAL *spec = CTX_VAR(ctx, STD_PORT_SPEC);

    REBCNT len;

    switch (VAL_WORD_SYM(verb)) {

    case SYM_REFLECT: {
        INCLUDE_PARAMS_OF_REFLECT;

        UNUSED(ARG(value));
        REBSYM property = VAL_WORD_SYM(ARG(property));
        assert(property != SYM_0);

        switch (property) {
        case SYM_OPEN_Q:
            return Init_Logic(D_OUT, did (sock->flags & RRF_OPEN));

        default:
            break;
        }

        break; }

    case SYM_READ: {
        INCLUDE_PARAMS_OF_READ;

        UNUSED(PAR(source));
        if (REF(part)) {
            UNUSED(ARG(limit));
            fail (Error_Bad_Refines_Raw());
        }

        if (REF(seek)) {
            UNUSED(ARG(index));
            fail (Error_Bad_Refines_Raw());
        }

        UNUSED(PAR(string)); // handled in dispatcher
        UNUSED(PAR(lines)); // handled in dispatcher

        if (not (sock->flags & RRF_OPEN))
            OS_DO_DEVICE_SYNC(sock, RDC_OPEN);

        arg = Obj_Value(spec, STD_PORT_SPEC_NET_HOST);

        // A DNS read e.g. of `read dns://66.249.66.140` should do a reverse
        // lookup.  The scheme handler may pass in either a TUPLE! or a string
        // that scans to a tuple, at this time (currently uses a string)
        //
        if (IS_TUPLE(arg)) {
            sock->modes |= RST_REVERSE;
            memcpy(&(DEVREQ_NET(sock)->remote_ip), VAL_TUPLE(arg), 4);
        }
        else if (IS_TEXT(arg)) {
            REBSIZ offset;
            REBSIZ size;
            REBSER *temp = Temp_UTF8_At_Managed(
                &offset, &size, arg, VAL_LEN_AT(arg)
            );

            DECLARE_LOCAL (tmp);
            if (Scan_Tuple(tmp, BIN_AT(temp, offset), size) != NULL) {
                sock->modes |= RST_REVERSE;
                memcpy(&(DEVREQ_NET(sock)->remote_ip), VAL_TUPLE(tmp), 4);
            }
            else // lookup string's IP address
                sock->common.data = VAL_BIN_HEAD(arg);
        }
        else
            fail (Error_On_Port(SYM_INVALID_SPEC, port, -10));

        OS_DO_DEVICE_SYNC(sock, RDC_READ);

        len = 1;
        goto pick; }

    case SYM_PICK: { // FIRST - return result
        if (not (sock->flags & RRF_OPEN))
            fail (Error_On_Port(SYM_NOT_OPEN, port, -12));

     pick:
        len = Get_Num_From_Arg(arg); // Position
        if (len != 1)
            fail (Error_Out_Of_Range(arg));

        assert(sock->flags & RRF_DONE); // R3-Alpha async DNS removed

        if (DEVREQ_NET(sock)->host_info == NULL) {
            Init_Blank(D_OUT); // HOST_NOT_FOUND or NO_ADDRESS blank vs. error
            return D_OUT; // READ action currently required to use D_OUT
        }

        if (sock->modes & RST_REVERSE) {
            Init_Text(
                D_OUT,
                Copy_Bytes(sock->common.data, LEN_BYTES(sock->common.data))
            );
        }
        else {
            Set_Tuple(D_OUT, cast(REBYTE*, &DEVREQ_NET(sock)->remote_ip), 4);
        }

        OS_DO_DEVICE_SYNC(sock, RDC_CLOSE);
        return D_OUT; }

    case SYM_OPEN: {
        INCLUDE_PARAMS_OF_OPEN;

        UNUSED(PAR(spec));
        if (REF(new))
            fail (Error_Bad_Refines_Raw());
        if (REF(read))
            fail (Error_Bad_Refines_Raw());
        if (REF(write))
            fail (Error_Bad_Refines_Raw());
        if (REF(seek))
            fail (Error_Bad_Refines_Raw());
        if (REF(allow)) {
            UNUSED(ARG(access));
            fail (Error_Bad_Refines_Raw());
        }

        OS_DO_DEVICE_SYNC(sock, RDC_OPEN);
        RETURN (port); }

    case SYM_CLOSE: {
        OS_DO_DEVICE_SYNC(sock, RDC_CLOSE);
        RETURN (port); }

    case SYM_ON_WAKE_UP:
        return Init_Bar(D_OUT);

    default:
        break;
    }

    fail (Error_Illegal_Action(REB_PORT, verb));
}


//
//  get-dns-actor-handle: native [
//
//  {Retrieve handle to the native actor for DNS}
//
//      return: [handle!]
//  ]
//
REBNATIVE(get_dns_actor_handle)
{
    Make_Port_Actor_Handle(D_OUT, &DNS_Actor);
    return D_OUT;
}
