import {loadFile, writeFile} from "qjs:std"

const cutils_h = loadFile("src/cutils.h")
const dtoa_c = loadFile("src/dtoa.c")
const dtoa_h = loadFile("src/dtoa.h")
const libregexp_c = loadFile("src/libregexp.c")
const libregexp_h = loadFile("src/libregexp.h")
const libregexp_opcode_h = loadFile("src/libregexp-opcode.h")
const libunicode_c = loadFile("src/libunicode.c")
const libunicode_h = loadFile("src/libunicode.h")
const libunicode_table_h = loadFile("src/libunicode-table.h")
const list_h = loadFile("src/list.h")
const quickjs_atom_h = loadFile("src/quickjs-atom.h")
const quickjs_compiler_c = loadFile("src/quickjs-compiler.c")
const quickjs_serialize_c = loadFile("src/quickjs-serialize.c")
const quickjs_runtime_c = loadFile("src/quickjs-runtime.c")
const quickjs_c_atomics_h = loadFile("src/quickjs-c-atomics.h")
const quickjs_h = loadFile("src/quickjs.h")
const quickjs_libc_c = loadFile("src/quickjs-libc.c")
const quickjs_libc_h = loadFile("src/quickjs-libc.h")
const quickjs_opcode_h = loadFile("src/quickjs-opcode.h")
const gen_builtin_array_fromasync_h = loadFile("builtin-array-fromasync.h")
const gen_builtin_iterator_zip_h = loadFile("builtin-iterator-zip.h")
const gen_builtin_iterator_zip_keyed_h = loadFile("builtin-iterator-zip-keyed.h")

let source = "#if defined(QJS_BUILD_LIBC) && defined(__linux__) && !defined(_GNU_SOURCE)\n"
           + "#define _GNU_SOURCE\n"
           + "#endif\n"
           + quickjs_c_atomics_h
           + cutils_h
           + dtoa_h
           + list_h
           + libunicode_h // exports lre_is_id_start, used by libregexp.h
           + libregexp_h
           + libunicode_table_h
           + quickjs_h
           + quickjs_compiler_c
           + quickjs_serialize_c
           + quickjs_runtime_c
           + dtoa_c
           + libregexp_c
           + libunicode_c
           + "#ifdef QJS_BUILD_LIBC\n"
           + quickjs_libc_h
           + quickjs_libc_c
           + "#endif // QJS_BUILD_LIBC\n"
source = source.replace(/#include "quickjs-atom.h"/g, quickjs_atom_h)
source = source.replace(/#include "quickjs-opcode.h"/g, quickjs_opcode_h)
source = source.replace(/#include "libregexp-opcode.h"/g, libregexp_opcode_h)
source = source.replace(/#include "builtin-array-fromasync.h"/g,
                        gen_builtin_array_fromasync_h)
source = source.replace(/#include "builtin-iterator-zip.h"/g,
                        gen_builtin_iterator_zip_h)
source = source.replace(/#include "builtin-iterator-zip-keyed.h"/g,
                        gen_builtin_iterator_zip_keyed_h)
source = source.replace(/#include "[^"]+"/g, "")
writeFile(execArgv[2] ?? "quickjs-amalgam.c", source)
