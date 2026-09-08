/*
 * QuickJS Javascript Engine
 *
 * Copyright (c) 2017-2026 Fabrice Bellard
 * Copyright (c) 2017-2025 Charlie Gordon
 * Copyright (c) 2023-2026 Ben Noordhuis
 * Copyright (c) 2023-2026 Saúl Ibarra Corretgé
 */

#include "quickjs.h"

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#define QJS_VERSION_STRING \
    STRINGIFY(QJS_VERSION_MAJOR) "." STRINGIFY(QJS_VERSION_MINOR) "." STRINGIFY(QJS_VERSION_PATCH) QJS_VERSION_SUFFIX

const char *JS_GetVersion(void)
{
    return QJS_VERSION_STRING;
}

#undef STRINGIFY
#undef STRINGIFY_
