//
//  File: %reb-host.h
//  Summary: "Include files for hosting"
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

// don't define REB_DEF, so it assumes non-core API pattern
// (e.g. calls APIs via struct, RL->rebXXX(), vs. directly as RL_rebXXX())
//
#include "reb-config.h"

#include <stdlib.h> // size_t and other types used in rebol.h
#include "pstdint.h" // polyfill <stdint.h> for pre-C99/C++11 compilers
#include "pstdbool.h" // polyfill <stdbool.h> for pre-C99/C++11 compilers
#if !defined(REBOL_IMPLICIT_END)
    #define REBOL_EXPLICIT_END // ensure core compiles with pre-C99/C++11
#endif
#include "rebol.h"

#include "reb-c.h" // API should only require <stdbool.h> and <stdint.h>

struct rebol_devreq;
typedef struct rebol_devreq REBREQ;
#include "reb-device.h"


#include "host-lib.h"
