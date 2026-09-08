/*******************************************************************/
/* object list */

typedef struct {
    JSObject *obj;
    uint32_t hash_next; /* -1 if no next entry */
} JSObjectListEntry;

/* XXX: reuse it to optimize weak references */
typedef struct {
    JSObjectListEntry *object_tab;
    int object_count;
    int object_size;
    uint32_t *hash_table;
    uint32_t hash_size;
} JSObjectList;

static void js_object_list_init(JSObjectList *s)
{
    memset(s, 0, sizeof(*s));
}

static uint32_t js_object_list_get_hash(JSObject *p, uint32_t hash_size)
{
    return ((uintptr_t)p * 3163) & (hash_size - 1);
}

static int js_object_list_resize_hash(JSContext *ctx, JSObjectList *s,
                                 uint32_t new_hash_size)
{
    JSObjectListEntry *e;
    uint32_t i, h, *new_hash_table;

    new_hash_table = js_malloc(ctx, sizeof(new_hash_table[0]) * new_hash_size);
    if (!new_hash_table)
        return -1;
    js_free(ctx, s->hash_table);
    s->hash_table = new_hash_table;
    s->hash_size = new_hash_size;

    for(i = 0; i < s->hash_size; i++) {
        s->hash_table[i] = -1;
    }
    for(i = 0; i < s->object_count; i++) {
        e = &s->object_tab[i];
        h = js_object_list_get_hash(e->obj, s->hash_size);
        e->hash_next = s->hash_table[h];
        s->hash_table[h] = i;
    }
    return 0;
}

/* the reference count of 'obj' is not modified. Return 0 if OK, -1 if
   memory error */
static int js_object_list_add(JSContext *ctx, JSObjectList *s, JSObject *obj)
{
    JSObjectListEntry *e;
    uint32_t h, new_hash_size;

    if (js_resize_array(ctx, (void *)&s->object_tab,
                        sizeof(s->object_tab[0]),
                        &s->object_size, s->object_count + 1))
        return -1;
    if (unlikely((s->object_count + 1) >= s->hash_size)) {
        new_hash_size = max_uint32(s->hash_size, 4);
        while (new_hash_size <= s->object_count)
            new_hash_size *= 2;
        if (js_object_list_resize_hash(ctx, s, new_hash_size))
            return -1;
    }
    e = &s->object_tab[s->object_count++];
    h = js_object_list_get_hash(obj, s->hash_size);
    e->obj = obj;
    e->hash_next = s->hash_table[h];
    s->hash_table[h] = s->object_count - 1;
    return 0;
}

/* return -1 if not present or the object index */
static int js_object_list_find(JSContext *ctx, JSObjectList *s, JSObject *obj)
{
    JSObjectListEntry *e;
    uint32_t h, p;

    /* must test empty size because there is no hash table */
    if (s->object_count == 0)
        return -1;
    h = js_object_list_get_hash(obj, s->hash_size);
    p = s->hash_table[h];
    while (p != -1) {
        e = &s->object_tab[p];
        if (e->obj == obj)
            return p;
        p = e->hash_next;
    }
    return -1;
}

static void js_object_list_end(JSContext *ctx, JSObjectList *s)
{
    js_free(ctx, s->object_tab);
    js_free(ctx, s->hash_table);
}

/*******************************************************************/
/* binary object writer & reader */

typedef enum BCTagEnum {
    BC_TAG_NULL = 1,
    BC_TAG_UNDEFINED,
    BC_TAG_BOOL_FALSE,
    BC_TAG_BOOL_TRUE,
    BC_TAG_INT32,
    BC_TAG_FLOAT64,
    BC_TAG_STRING,
    BC_TAG_OBJECT,
    BC_TAG_ARRAY,
    BC_TAG_BIG_INT,
    BC_TAG_TEMPLATE_OBJECT,
    BC_TAG_FUNCTION_BYTECODE,
    BC_TAG_MODULE,
    BC_TAG_TYPED_ARRAY,
    BC_TAG_ARRAY_BUFFER,
    BC_TAG_SHARED_ARRAY_BUFFER,
    BC_TAG_REGEXP,
    BC_TAG_DATE,
    BC_TAG_OBJECT_VALUE,
    BC_TAG_OBJECT_REFERENCE,
    BC_TAG_MAP,
    BC_TAG_SET,
    BC_TAG_SYMBOL,
} BCTagEnum;

#define BC_VERSION 27

typedef struct BCWriterState {
    JSContext *ctx;
    DynBuf dbuf;
    bool allow_bytecode;
    bool allow_sab;
    bool allow_reference;
    bool allow_source;
    bool allow_debug;
    uint32_t first_atom;
    uint32_t *atom_to_idx;
    int atom_to_idx_size;
    JSAtom *idx_to_atom;
    int idx_to_atom_count;
    int idx_to_atom_size;
    uint8_t **sab_tab;
    int sab_tab_len;
    int sab_tab_size;
    /* list of referenced objects (used if allow_reference = true) */
    JSObjectList object_list;
} BCWriterState;

#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
static const char * const bc_tag_str[] = {
    "invalid",
    "null",
    "undefined",
    "false",
    "true",
    "int32",
    "float64",
    "string",
    "object",
    "array",
    "BigInt",
    "template",
    "function",
    "module",
    "TypedArray",
    "ArrayBuffer",
    "SharedArrayBuffer",
    "RegExp",
    "Date",
    "ObjectValue",
    "ObjectReference",
    "Map",
    "Set",
    "Symbol",
};

static const char *bc_tag_name(uint8_t tag)
{
    if (tag >= countof(bc_tag_str))
        return "<bad tag>";
    return bc_tag_str[tag];
}
#endif

static void bc_put_u8(BCWriterState *s, uint8_t v)
{
    dbuf_putc(&s->dbuf, v);
}

static void bc_put_u16(BCWriterState *s, uint16_t v)
{
    dbuf_put_u16(&s->dbuf, v);
}

static __maybe_unused void bc_put_u32(BCWriterState *s, uint32_t v)
{
    dbuf_put_u32(&s->dbuf, v);
}

static void bc_put_u64(BCWriterState *s, uint64_t v)
{
    dbuf_put(&s->dbuf, (uint8_t *)&v, sizeof(v));
}

static void bc_put_leb128(BCWriterState *s, uint32_t v)
{
    dbuf_put_leb128(&s->dbuf, v);
}

static void bc_put_sleb128(BCWriterState *s, int32_t v)
{
    dbuf_put_sleb128(&s->dbuf, v);
}

static void bc_set_flags(uint32_t *pflags, int *pidx, uint32_t val, int n)
{
    *pflags = *pflags | (val << *pidx);
    *pidx += n;
}

static int bc_atom_to_idx(BCWriterState *s, uint32_t *pres, JSAtom atom)
{
    uint32_t v;

    if (atom < s->first_atom || __JS_AtomIsTaggedInt(atom)) {
        *pres = atom;
        return 0;
    }
    atom -= s->first_atom;
    if (atom < s->atom_to_idx_size && s->atom_to_idx[atom] != 0) {
        *pres = s->atom_to_idx[atom];
        return 0;
    }
    if (atom >= s->atom_to_idx_size) {
        int old_size, i;
        old_size = s->atom_to_idx_size;
        if (js_resize_array(s->ctx, (void **)&s->atom_to_idx,
                            sizeof(s->atom_to_idx[0]), &s->atom_to_idx_size,
                            atom + 1))
            return -1;
        /* XXX: could add a specific js_resize_array() function to do it */
        for(i = old_size; i < s->atom_to_idx_size; i++)
            s->atom_to_idx[i] = 0;
    }
    if (js_resize_array(s->ctx, (void **)&s->idx_to_atom,
                        sizeof(s->idx_to_atom[0]),
                        &s->idx_to_atom_size, s->idx_to_atom_count + 1))
        goto fail;

    v = s->idx_to_atom_count++;
    s->idx_to_atom[v] = atom + s->first_atom;
    v += s->first_atom;
    s->atom_to_idx[atom] = v;
    *pres = v;
    return 0;
 fail:
    *pres = 0;
    return -1;
}

static int bc_put_atom(BCWriterState *s, JSAtom atom)
{
    uint32_t v;

    if (__JS_AtomIsTaggedInt(atom)) {
        v = (__JS_AtomToUInt32(atom) << 1) | 1;
    } else {
        if (bc_atom_to_idx(s, &v, atom))
            return -1;
        v <<= 1;
    }
    bc_put_leb128(s, v);
    return 0;
}

static uint32_t bc_csum(const uint8_t *p, size_t n)
{
    uint32_t a, b, c, h;
    size_t i;

    h = 0;
    for (i = 0; i+4 < n; i += 4) {
        h += get_u32(p+i);
        h *= 0x9e370001;
    }
    a = b = c = 0;
    switch (n-i) {
    case 3:
        c = (uint32_t)p[i+2];
    case 2:
        b = (uint32_t)p[i+1];
    case 1:
        a = (uint32_t)p[i+0];
    case 0:
        break;
    }
    h += a | b<<8 | c<<16;
    h *= 0x9e370001;
    return h;
}

static int JS_WriteFunctionBytecode(BCWriterState *s,
                                    const JSFunctionBytecode *b)
{
    int pos, len, bc_len, op;
    JSAtom atom;
    uint8_t *bc_buf;
    uint32_t val;

    bc_len = b->byte_code_len;
    bc_buf = js_malloc(s->ctx, bc_len);
    if (!bc_buf)
        return -1;
    memcpy(bc_buf, b->byte_code_buf, bc_len);

    pos = 0;
    while (pos < bc_len) {
        op = bc_buf[pos];
        len = short_opcode_info(op).size;
        switch(short_opcode_info(op).fmt) {
        case OP_FMT_atom:
        case OP_FMT_atom_u8:
        case OP_FMT_atom_u16:
        case OP_FMT_atom_label_u8:
        case OP_FMT_atom_label_u16:
            atom = get_u32(bc_buf + pos + 1);
            if (bc_atom_to_idx(s, &val, atom))
                goto fail;
            put_u32(bc_buf + pos + 1, val);
            break;
        default:
            break;
        }
        pos += len;
    }

    dbuf_put(&s->dbuf, bc_buf, bc_len);

    js_free(s->ctx, bc_buf);
    return 0;
 fail:
    js_free(s->ctx, bc_buf);
    return -1;
}

static void JS_WriteString(BCWriterState *s, JSString *p)
{
    int i;
    bc_put_leb128(s, ((uint32_t)p->len << 1) | p->is_wide_char);
    if (p->is_wide_char) {
        for(i = 0; i < p->len; i++)
            bc_put_u16(s, str16(p)[i]);
    } else {
        dbuf_put(&s->dbuf, str8(p), p->len);
    }
}

static int JS_WriteBigInt(BCWriterState *s, JSValueConst obj)
{
    JSBigIntBuf buf;
    JSBigInt *p;
    uint32_t len, i;
    js_limb_t v, b;
    int shift;

    bc_put_u8(s, BC_TAG_BIG_INT);

    if (JS_VALUE_GET_TAG(obj) == JS_TAG_SHORT_BIG_INT)
        p = js_bigint_set_short(&buf, obj);
    else
        p = JS_VALUE_GET_PTR(obj);
    if (p->len == 1 && p->tab[0] == 0) {
        /* zero case */
        len = 0;
    } else {
        /* compute the length of the two's complement representation
           in bytes */
        len = p->len * (JS_LIMB_BITS / 8);
        v = p->tab[p->len - 1];
        shift = JS_LIMB_BITS - 8;
        while (shift > 0) {
            b = (v >> shift) & 0xff;
            if (b != 0x00 && b != 0xff)
                break;
            if ((b & 1) != ((v >> (shift - 1)) & 1))
                break;
            shift -= 8;
            len--;
        }
    }
    bc_put_leb128(s, len);
    if (len > 0) {
        for(i = 0; i < (len / (JS_LIMB_BITS / 8)); i++) {
            bc_put_u32(s, p->tab[i]);
        }
        for(i = 0; i < len % (JS_LIMB_BITS / 8); i++) {
            bc_put_u8(s, (p->tab[p->len - 1] >> (i * 8)) & 0xff);
        }
    }
    return 0;
}

static int JS_WriteObjectRec(BCWriterState *s, JSValueConst obj);

static int JS_WriteFunctionTag(BCWriterState *s, JSValueConst obj)
{
    JSFunctionBytecode *b = JS_VALUE_GET_PTR(obj);
    uint32_t flags;
    int idx, i;

    bc_put_u8(s, BC_TAG_FUNCTION_BYTECODE);
    flags = idx = 0;
    bc_set_flags(&flags, &idx, b->has_prototype, 1);
    bc_set_flags(&flags, &idx, b->has_simple_parameter_list, 1);
    bc_set_flags(&flags, &idx, b->is_derived_class_constructor, 1);
    bc_set_flags(&flags, &idx, b->need_home_object, 1);
    bc_set_flags(&flags, &idx, b->func_kind, 2);
    bc_set_flags(&flags, &idx, b->new_target_allowed, 1);
    bc_set_flags(&flags, &idx, b->super_call_allowed, 1);
    bc_set_flags(&flags, &idx, b->super_allowed, 1);
    bc_set_flags(&flags, &idx, b->arguments_allowed, 1);
    bc_set_flags(&flags, &idx, b->backtrace_barrier, 1);
    bc_set_flags(&flags, &idx, s->allow_debug, 1);
    assert(idx <= 16);
    bc_put_u16(s, flags);
    bc_put_u8(s, b->is_strict_mode);
    bc_put_atom(s, b->func_name);

    bc_put_leb128(s, b->arg_count);
    bc_put_leb128(s, b->var_count);
    bc_put_leb128(s, b->defined_arg_count);
    bc_put_leb128(s, b->stack_size);
    bc_put_leb128(s, b->var_ref_count);
    bc_put_leb128(s, b->closure_var_count);
    bc_put_leb128(s, b->cpool_count);
    bc_put_leb128(s, b->byte_code_len);
    if (b->vardefs) {
        /* XXX: this field is redundant */
        bc_put_leb128(s, b->arg_count + b->var_count);
        for(i = 0; i < b->arg_count + b->var_count; i++) {
            JSVarDef *vd = &b->vardefs[i];
            bc_put_atom(s, vd->var_name);
            bc_put_leb128(s, vd->scope_level);
            bc_put_leb128(s, vd->scope_next + 1);
            flags = idx = 0;
            bc_set_flags(&flags, &idx, vd->var_kind, 4);
            bc_set_flags(&flags, &idx, vd->is_const, 1);
            bc_set_flags(&flags, &idx, vd->is_lexical, 1);
            bc_set_flags(&flags, &idx, vd->is_captured, 1);
            assert(idx <= 8);
            bc_put_u8(s, flags);
            if (vd->is_captured)
                bc_put_leb128(s, vd->var_ref_idx);
        }
    } else {
        bc_put_leb128(s, 0);
    }

    for(i = 0; i < b->closure_var_count; i++) {
        JSClosureVar *cv = &b->closure_var[i];
        bc_put_atom(s, cv->var_name);
        bc_put_leb128(s, cv->var_idx);
        flags = idx = 0;
        bc_set_flags(&flags, &idx, cv->closure_type, 3);
        bc_set_flags(&flags, &idx, cv->is_const, 1);
        bc_set_flags(&flags, &idx, cv->is_lexical, 1);
        bc_set_flags(&flags, &idx, cv->var_kind, 4);
        assert(idx <= 16);
        bc_put_leb128(s, flags);
    }

    // write constant pool before code so code can be disassembled
    // on the fly at read time
    for(i = 0; i < b->cpool_count; i++) {
        if (JS_WriteObjectRec(s, b->cpool[i]))
            goto fail;
    }

    if (JS_WriteFunctionBytecode(s, b))
        goto fail;

    if (s->allow_debug) {
        bc_put_atom(s, b->filename);
        bc_put_leb128(s, b->line_num);
        bc_put_leb128(s, b->col_num);
        bc_put_leb128(s, b->pc2line_len);
        dbuf_put(&s->dbuf, b->pc2line_buf, b->pc2line_len);
        if (s->allow_source && b->source) {
            bc_put_leb128(s, b->source_len);
            dbuf_put(&s->dbuf, b->source, b->source_len);
        } else {
            bc_put_leb128(s, 0);
        }
    }
    return 0;
 fail:
    return -1;
}

static int JS_WriteModule(BCWriterState *s, JSValueConst obj)
{
    JSModuleDef *m = JS_VALUE_GET_PTR(obj);
    int i;

    bc_put_u8(s, BC_TAG_MODULE);
    bc_put_atom(s, m->module_name);

    bc_put_leb128(s, m->req_module_entries_count);
    for(i = 0; i < m->req_module_entries_count; i++) {
        JSReqModuleEntry *rme = &m->req_module_entries[i];
        bc_put_atom(s, rme->module_name);
    }

    bc_put_leb128(s, m->export_entries_count);
    for(i = 0; i < m->export_entries_count; i++) {
        JSExportEntry *me = &m->export_entries[i];
        bc_put_u8(s, me->export_type);
        if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
            bc_put_leb128(s, me->u.local.var_idx);
        } else {
            bc_put_leb128(s, me->u.req_module_idx);
            bc_put_atom(s, me->local_name);
        }
        bc_put_atom(s, me->export_name);
    }

    bc_put_leb128(s, m->star_export_entries_count);
    for(i = 0; i < m->star_export_entries_count; i++) {
        JSStarExportEntry *se = &m->star_export_entries[i];
        bc_put_leb128(s, se->req_module_idx);
    }

    bc_put_leb128(s, m->import_entries_count);
    for(i = 0; i < m->import_entries_count; i++) {
        JSImportEntry *mi = &m->import_entries[i];
        bc_put_leb128(s, mi->var_idx);
        bc_put_atom(s, mi->import_name);
        bc_put_leb128(s, mi->req_module_idx);
    }

    bc_put_u8(s, m->has_tla);

    if (JS_WriteObjectRec(s, m->func_obj))
        goto fail;
    return 0;
 fail:
    return -1;
}

static int JS_WriteArray(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    uint32_t i, len;
    JSValue val;
    int ret;
    bool is_template;

    if (s->allow_bytecode && !p->extensible) {
        /* not extensible array: we consider it is a
           template when we are saving bytecode */
        bc_put_u8(s, BC_TAG_TEMPLATE_OBJECT);
        is_template = true;
    } else {
        bc_put_u8(s, BC_TAG_ARRAY);
        is_template = false;
    }
    if (js_get_length32(s->ctx, &len, obj))
        goto fail1;
    bc_put_leb128(s, len);
    for(i = 0; i < len; i++) {
        val = JS_GetPropertyUint32(s->ctx, obj, i);
        if (JS_IsException(val))
            goto fail1;
        ret = JS_WriteObjectRec(s, val);
        JS_FreeValue(s->ctx, val);
        if (ret)
            goto fail1;
    }
    if (is_template) {
        val = JS_GetProperty(s->ctx, obj, JS_ATOM_raw);
        if (JS_IsException(val))
            goto fail1;
        ret = JS_WriteObjectRec(s, val);
        JS_FreeValue(s->ctx, val);
        if (ret)
            goto fail1;
    }
    return 0;
 fail1:
    return -1;
}

static int JS_WriteObjectTag(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    uint32_t i, prop_count;
    JSShape *sh;
    JSShapeProperty *pr;
    int pass;
    JSAtom atom;

    bc_put_u8(s, BC_TAG_OBJECT);
    prop_count = 0;
    sh = p->shape;
    for(pass = 0; pass < 2; pass++) {
        if (pass == 1)
            bc_put_leb128(s, prop_count);
        for(i = 0, pr = get_shape_prop(sh); i < sh->prop_count; i++, pr++) {
            atom = pr->atom;
            if (atom != JS_ATOM_NULL && (pr->flags & JS_PROP_ENUMERABLE)) {
                if (pr->flags & JS_PROP_TMASK) {
                    JS_ThrowTypeError(s->ctx, "only value properties are supported");
                    goto fail;
                }
                if (pass == 0) {
                    prop_count++;
                } else {
                    bc_put_atom(s, atom);
                    if (JS_WriteObjectRec(s, p->prop[i].u.value))
                        goto fail;
                }
            }
        }
    }
    return 0;
 fail:
    return -1;
}

static int JS_WriteTypedArray(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSTypedArray *ta = p->u.typed_array;

    bc_put_u8(s, BC_TAG_TYPED_ARRAY);
    bc_put_u8(s, p->class_id - JS_CLASS_UINT8C_ARRAY);
    bc_put_leb128(s, p->u.array.count);
    bc_put_leb128(s, ta->offset);
    if (JS_WriteObjectRec(s, JS_MKPTR(JS_TAG_OBJECT, ta->buffer)))
        return -1;
    return 0;
}

static int JS_WriteArrayBuffer(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSArrayBuffer *abuf = p->u.array_buffer;
    if (abuf->detached) {
        JS_ThrowTypeErrorDetachedArrayBuffer(s->ctx);
        return -1;
    }
    bc_put_u8(s, BC_TAG_ARRAY_BUFFER);
    bc_put_leb128(s, abuf->byte_length);
    bc_put_leb128(s, abuf->max_byte_length);
    dbuf_put(&s->dbuf, abuf->data, abuf->byte_length);
    return 0;
}

static int JS_WriteSharedArrayBuffer(BCWriterState *s, JSValueConst obj)
{
    JSObject *p = JS_VALUE_GET_OBJ(obj);
    JSArrayBuffer *abuf = p->u.array_buffer;
    assert(!abuf->detached); /* SharedArrayBuffer are never detached */
    bc_put_u8(s, BC_TAG_SHARED_ARRAY_BUFFER);
    bc_put_leb128(s, abuf->byte_length);
    bc_put_leb128(s, abuf->max_byte_length);
    bc_put_u64(s, (uintptr_t)abuf->data);
    if (js_resize_array(s->ctx, (void **)&s->sab_tab, sizeof(s->sab_tab[0]),
                        &s->sab_tab_size, s->sab_tab_len + 1))
        return -1;
    /* keep the SAB pointer so that the user can clone it or free it */
    s->sab_tab[s->sab_tab_len++] = abuf->data;
    return 0;
}

static int JS_WriteRegExp(BCWriterState *s, JSRegExp regexp)
{
    JSString *bc = regexp.bytecode;
    assert(!bc->is_wide_char);
    JS_WriteString(s, regexp.pattern);
    JS_WriteString(s, bc);
    return 0;
}

static int JS_WriteMap(BCWriterState *s, struct JSMapState *map_state);
static int JS_WriteSet(BCWriterState *s, struct JSMapState *map_state);

static int JS_WriteObjectRec(BCWriterState *s, JSValueConst obj)
{
    uint32_t tag;

    if (js_check_stack_overflow(s->ctx->rt, 0)) {
        JS_ThrowStackOverflow(s->ctx);
        return -1;
    }

    tag = JS_VALUE_GET_NORM_TAG(obj);
    switch(tag) {
    case JS_TAG_NULL:
        bc_put_u8(s, BC_TAG_NULL);
        break;
    case JS_TAG_UNDEFINED:
        bc_put_u8(s, BC_TAG_UNDEFINED);
        break;
    case JS_TAG_BOOL:
        bc_put_u8(s, BC_TAG_BOOL_FALSE + JS_VALUE_GET_INT(obj));
        break;
    case JS_TAG_INT:
        bc_put_u8(s, BC_TAG_INT32);
        bc_put_sleb128(s, JS_VALUE_GET_INT(obj));
        break;
    case JS_TAG_FLOAT64:
        {
            JSFloat64Union u;
            bc_put_u8(s, BC_TAG_FLOAT64);
            u.d = JS_VALUE_GET_FLOAT64(obj);
            bc_put_u64(s, u.u64);
        }
        break;
    case JS_TAG_STRING:
        {
            JSString *p = JS_VALUE_GET_STRING(obj);
            bc_put_u8(s, BC_TAG_STRING);
            JS_WriteString(s, p);
        }
        break;
    case JS_TAG_STRING_ROPE:
        {
            JSValue str;
            int ret;
            str = JS_ToString(s->ctx, obj);
            if (JS_IsException(str))
                goto fail;
            ret = JS_WriteObjectRec(s, str);
            JS_FreeValue(s->ctx, str);
            if (ret)
                goto fail;
        }
        break;
    case JS_TAG_FUNCTION_BYTECODE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        if (JS_WriteFunctionTag(s, obj))
            goto fail;
        break;
    case JS_TAG_MODULE:
        if (!s->allow_bytecode)
            goto invalid_tag;
        if (JS_WriteModule(s, obj))
            goto fail;
        break;
    case JS_TAG_OBJECT:
        {
            JSObject *p = JS_VALUE_GET_OBJ(obj);
            int ret, idx;

            if (s->allow_reference) {
                idx = js_object_list_find(s->ctx, &s->object_list, p);
                if (idx >= 0) {
                    bc_put_u8(s, BC_TAG_OBJECT_REFERENCE);
                    bc_put_leb128(s, idx);
                    break;
                } else {
                    if (js_object_list_add(s->ctx, &s->object_list, p))
                        goto fail;
                }
            } else {
                if (p->tmp_mark) {
                    JS_ThrowTypeError(s->ctx, "circular reference");
                    goto fail;
                }
                p->tmp_mark = 1;
            }
            switch(p->class_id) {
            case JS_CLASS_ARRAY:
                ret = JS_WriteArray(s, obj);
                break;
            case JS_CLASS_OBJECT:
                ret = JS_WriteObjectTag(s, obj);
                break;
            case JS_CLASS_ARRAY_BUFFER:
                ret = JS_WriteArrayBuffer(s, obj);
                break;
            case JS_CLASS_SHARED_ARRAY_BUFFER:
                if (!s->allow_sab)
                    goto invalid_tag;
                ret = JS_WriteSharedArrayBuffer(s, obj);
                break;
            case JS_CLASS_REGEXP:
                bc_put_u8(s, BC_TAG_REGEXP);
                ret = JS_WriteRegExp(s, p->u.regexp);
                break;
            case JS_CLASS_DATE:
                bc_put_u8(s, BC_TAG_DATE);
                ret = JS_WriteObjectRec(s, p->u.object_data);
                break;
            case JS_CLASS_NUMBER:
            case JS_CLASS_STRING:
            case JS_CLASS_BOOLEAN:
            case JS_CLASS_BIG_INT:
                bc_put_u8(s, BC_TAG_OBJECT_VALUE);
                ret = JS_WriteObjectRec(s, p->u.object_data);
                break;
            case JS_CLASS_MAP:
                bc_put_u8(s, BC_TAG_MAP);
                ret = JS_WriteMap(s, p->u.map_state);
                break;
            case JS_CLASS_SET:
                bc_put_u8(s, BC_TAG_SET);
                ret = JS_WriteSet(s, p->u.map_state);
                break;
            default:
                if (is_typed_array(p->class_id)) {
                    ret = JS_WriteTypedArray(s, obj);
                } else {
                    JS_ThrowTypeError(s->ctx, "unsupported object class");
                    ret = -1;
                }
                break;
            }
            p->tmp_mark = 0;
            if (ret)
                goto fail;
        }
        break;
    case JS_TAG_SHORT_BIG_INT:
    case JS_TAG_BIG_INT:
        if (JS_WriteBigInt(s, obj))
            goto fail;
        break;
    case JS_TAG_SYMBOL:
        {
            JSAtomStruct *p = JS_VALUE_GET_PTR(obj);
            if (p->atom_type != JS_ATOM_TYPE_GLOBAL_SYMBOL && p->atom_type != JS_ATOM_TYPE_SYMBOL) {
                JS_ThrowTypeError(s->ctx, "unsupported symbol type");
                goto fail;
            }
            JSAtom atom = js_get_atom_index(s->ctx->rt, p);
            bc_put_u8(s, BC_TAG_SYMBOL);
            bc_put_atom(s, atom);
        }
        break;
    default:
    invalid_tag:
        JS_ThrowInternalError(s->ctx, "unsupported tag (%d)", tag);
        goto fail;
    }
    return 0;

 fail:
    return -1;
}

/* create the atom table */
static int JS_WriteObjectAtoms(BCWriterState *s)
{
    JSRuntime *rt = s->ctx->rt;
    DynBuf dbuf1;
    int i, atoms_size;

    dbuf1 = s->dbuf;
    js_dbuf_init(s->ctx, &s->dbuf);
    bc_put_u8(s, BC_VERSION);
    bc_put_u32(s, 0); // checksum, filled in after serialization

    bc_put_leb128(s, s->idx_to_atom_count);
    for(i = 0; i < s->idx_to_atom_count; i++) {
        JSAtom atom = s->idx_to_atom[i];
        if (__JS_AtomIsConst(atom)) {
            bc_put_u8(s, 0 /* the type */);
            /* TODO(saghul): encoding for tagged integers and keyword-ish atoms could be
               more efficient. */
            bc_put_u32(s, atom);
        } else {
            JSAtomStruct *p = rt->atom_array[atom];
            uint8_t type = p->atom_type;
            assert(type != JS_ATOM_TYPE_PRIVATE);
            bc_put_u8(s, type);
            JS_WriteString(s, p);
        }
    }
    /* XXX: should check for OOM in above phase */

    /* move the atoms at the start */
    /* XXX: could just append dbuf1 data, but it uses more memory if
       dbuf1 is larger than dbuf */
    atoms_size = s->dbuf.size;
    if (dbuf_claim(&dbuf1, atoms_size))
        goto fail;
    memmove(dbuf1.buf + atoms_size, dbuf1.buf, dbuf1.size);
    memcpy(dbuf1.buf, s->dbuf.buf, atoms_size);
    dbuf1.size += atoms_size;
    dbuf_free(&s->dbuf);
    s->dbuf = dbuf1;
    return 0;
 fail:
    dbuf_free(&dbuf1);
    return -1;
}

uint8_t *JS_WriteObject2(JSContext *ctx, size_t *psize, JSValueConst obj,
                         int flags, JSSABTab *psab_tab)
{
    BCWriterState ss, *s = &ss;
    uint32_t h;
    DynBuf *d;

    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->allow_bytecode = ((flags & JS_WRITE_OBJ_BYTECODE) != 0);
    s->allow_sab = ((flags & JS_WRITE_OBJ_SAB) != 0);
    s->allow_reference = ((flags & JS_WRITE_OBJ_REFERENCE) != 0);
    s->allow_source = ((flags & JS_WRITE_OBJ_STRIP_SOURCE) == 0);
    s->allow_debug = ((flags & JS_WRITE_OBJ_STRIP_DEBUG) == 0);
    /* XXX: could use a different version when bytecode is included */
    if (s->allow_bytecode)
        s->first_atom = JS_ATOM_END;
    else
        s->first_atom = 1;
    js_dbuf_init(ctx, &s->dbuf);
    js_object_list_init(&s->object_list);

    if (JS_WriteObjectRec(s, obj))
        goto fail;
    if (JS_WriteObjectAtoms(s))
        goto fail;
    js_object_list_end(ctx, &s->object_list);
    js_free(ctx, s->atom_to_idx);
    js_free(ctx, s->idx_to_atom);
    *psize = s->dbuf.size;
    if (psab_tab) {
        psab_tab->tab = s->sab_tab;
        psab_tab->len = s->sab_tab_len;
    } else {
        js_free(ctx, s->sab_tab);
    }
    // don't include version and checksum fields in checksum
    d = &s->dbuf;
    h = bc_csum(&d->buf[5], d->size - 5);
    put_u32(&d->buf[1], h);
    return d->buf;
 fail:
    js_object_list_end(ctx, &s->object_list);
    js_free(ctx, s->atom_to_idx);
    js_free(ctx, s->idx_to_atom);
    dbuf_free(&s->dbuf);
    *psize = 0;
    if (psab_tab) {
        psab_tab->tab = NULL;
        psab_tab->len = 0;
    }
    return NULL;
}

uint8_t *JS_WriteObject(JSContext *ctx, size_t *psize, JSValueConst obj,
                        int flags)
{
    return JS_WriteObject2(ctx, psize, obj, flags, NULL);
}

typedef struct BCReaderState {
    JSContext *ctx;
    const uint8_t *buf_start, *ptr, *buf_end;
    uint32_t first_atom;
    uint32_t idx_to_atom_count;
    JSAtom *idx_to_atom;
    int error_state;
    bool allow_sab;
    bool allow_bytecode;
    bool allow_reference;
    /* object references */
    JSObject **objects;
    int objects_count;
    int objects_size;
    /* SAB references */
    uint8_t **sab_tab;
    int sab_tab_len;
    int sab_tab_size;
    /* used for JS_DUMP_READ_OBJECT */
    const uint8_t *ptr_last;
    int level;
} BCReaderState;

#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
static void JS_PRINTF_FORMAT_ATTR(2, 3) bc_read_trace(BCReaderState *s, JS_PRINTF_FORMAT const char *fmt, ...) {
    va_list ap;
    int i, n, n0;

    if (!check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT))
        return;

    if (!s->ptr_last)
        s->ptr_last = s->buf_start;

    n = n0 = 0;
    if (s->ptr > s->ptr_last || s->ptr == s->buf_start) {
        n0 = printf("%04x: ", (int)(s->ptr_last - s->buf_start));
        n += n0;
    }
    for (i = 0; s->ptr_last < s->ptr; i++) {
        if ((i & 7) == 0 && i > 0) {
            printf("\n%*s", n0, "");
            n = n0;
        }
        n += printf(" %02x", *s->ptr_last++);
    }
    if (*fmt == '}')
        s->level--;
    if (n < 32 + s->level * 2) {
        printf("%*s", 32 + s->level * 2 - n, "");
    }
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (strchr(fmt, '{'))
        s->level++;
}
#else
#define bc_read_trace(...)
#endif

static int bc_read_error_end(BCReaderState *s)
{
    if (!s->error_state) {
        JS_ThrowSyntaxError(s->ctx, "read after the end of the buffer");
    }
    return s->error_state = -1;
}

static int bc_get_u8(BCReaderState *s, uint8_t *pval)
{
    if (unlikely(s->buf_end - s->ptr < 1)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    *pval = *s->ptr++;
    return 0;
}

static int bc_get_u16(BCReaderState *s, uint16_t *pval)
{
    uint16_t v;
    if (unlikely(s->buf_end - s->ptr < 2)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    v = get_u16(s->ptr);
    *pval = v;
    s->ptr += 2;
    return 0;
}

static __maybe_unused int bc_get_u32(BCReaderState *s, uint32_t *pval)
{
    uint32_t v;
    if (unlikely(s->buf_end - s->ptr < 4)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    v = get_u32(s->ptr);
    *pval = v;
    s->ptr += 4;
    return 0;
}

static int bc_get_u64(BCReaderState *s, uint64_t *pval)
{
    uint64_t v;
    if (unlikely(s->buf_end - s->ptr < 8)) {
        *pval = 0; /* avoid warning */
        return bc_read_error_end(s);
    }
    v = get_u64(s->ptr);
    *pval = v;
    s->ptr += 8;
    return 0;
}

static int bc_get_leb128(BCReaderState *s, uint32_t *pval)
{
    int ret;
    ret = get_leb128(pval, s->ptr, s->buf_end);
    if (unlikely(ret < 0))
        return bc_read_error_end(s);
    s->ptr += ret;
    return 0;
}

static int bc_get_sleb128(BCReaderState *s, int32_t *pval)
{
    int ret;
    ret = get_sleb128(pval, s->ptr, s->buf_end);
    if (unlikely(ret < 0))
        return bc_read_error_end(s);
    s->ptr += ret;
    return 0;
}

/* XXX: used to read an `int` with a positive value */
static int bc_get_leb128_int(BCReaderState *s, int *pval)
{
    return bc_get_leb128(s, (uint32_t *)pval);
}

static int bc_get_leb128_u16(BCReaderState *s, uint16_t *pval)
{
    uint32_t val;
    if (bc_get_leb128(s, &val)) {
        *pval = 0;
        return -1;
    }
    *pval = val;
    return 0;
}

static int bc_get_buf(BCReaderState *s, void *buf, uint32_t buf_len)
{
    if (buf_len != 0) {
        if (unlikely(!buf || s->buf_end - s->ptr < buf_len))
            return bc_read_error_end(s);
        memcpy(buf, s->ptr, buf_len);
        s->ptr += buf_len;
    }
    return 0;
}

static int bc_idx_to_atom(BCReaderState *s, JSAtom *patom, uint32_t idx)
{
    JSAtom atom;

    if (__JS_AtomIsTaggedInt(idx)) {
        atom = idx;
    } else if (idx < s->first_atom) {
        atom = JS_DupAtom(s->ctx, idx);
    } else {
        idx -= s->first_atom;
        if (idx >= s->idx_to_atom_count) {
            JS_ThrowSyntaxError(s->ctx, "invalid atom index (pos=%u)",
                                (unsigned int)(s->ptr - s->buf_start));
            *patom = JS_ATOM_NULL;
            return s->error_state = -1;
        }
        atom = JS_DupAtom(s->ctx, s->idx_to_atom[idx]);
    }
    *patom = atom;
    return 0;
}

static int bc_get_atom(BCReaderState *s, JSAtom *patom)
{
    uint32_t v;
    if (bc_get_leb128(s, &v))
        return -1;
    if (v & 1) {
        *patom = __JS_AtomFromUInt32(v >> 1);
        return 0;
    } else {
        return bc_idx_to_atom(s, patom, v >> 1);
    }
}

static JSString *JS_ReadString(BCReaderState *s)
{
    uint32_t len;
    size_t size;
    bool is_wide_char;
    JSString *p;

    if (bc_get_leb128(s, &len))
        return NULL;
    is_wide_char = len & 1;
    len >>= 1;
    if (len > JS_STRING_LEN_MAX) {
        JS_ThrowInternalError(s->ctx, "string too long");
        return NULL;
    }
    p = js_alloc_string(s->ctx, len, is_wide_char);
    if (!p) {
        s->error_state = -1;
        return NULL;
    }
    size = (size_t)len << is_wide_char;
    if ((s->buf_end - s->ptr) < size) {
        bc_read_error_end(s);
        js_free_string(s->ctx->rt, p);
        return NULL;
    }
    memcpy(str8(p), s->ptr, size);
    s->ptr += size;
    if (!is_wide_char)
        str8(p)[size] = '\0'; /* add the trailing zero for 8 bit strings */
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        bc_read_trace(s, "%s", ""); // hex dump and indentation
        JS_DumpString(s->ctx->rt, p);
        printf("\n");
    }
#endif
    return p;
}

static uint32_t bc_get_flags(uint32_t flags, int *pidx, int n)
{
    uint32_t val;
    /* XXX: this does not work for n == 32 */
    val = (flags >> *pidx) & ((1U << n) - 1);
    *pidx += n;
    return val;
}

static int JS_ReadFunctionBytecode(BCReaderState *s, JSFunctionBytecode *b,
                                   int byte_code_offset, uint32_t bc_len)
{
    uint8_t *bc_buf;
    int pos, len, op;
    JSAtom atom;
    uint32_t idx;

    bc_buf = (uint8_t*)b + byte_code_offset;
    if (bc_get_buf(s, bc_buf, bc_len))
        return -1;
    b->byte_code_buf = bc_buf;

    pos = 0;
    while (pos < bc_len) {
        op = bc_buf[pos];
        len = short_opcode_info(op).size;
        switch(short_opcode_info(op).fmt) {
        case OP_FMT_atom:
        case OP_FMT_atom_u8:
        case OP_FMT_atom_u16:
        case OP_FMT_atom_label_u8:
        case OP_FMT_atom_label_u16:
            idx = get_u32(bc_buf + pos + 1);
            if (bc_idx_to_atom(s, &atom, idx)) {
                /* Note: the atoms will be freed up to this position */
                b->byte_code_len = pos;
                return -1;
            }
            put_u32(bc_buf + pos + 1, atom);
            break;
        default:
            break;
        }
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
        if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
            const uint8_t *save_ptr = s->ptr;
            s->ptr = s->ptr_last + len;
            s->level -= 4;
            bc_read_trace(s, "%s", "");  // hex dump + indent
            dump_single_byte_code(s->ctx, bc_buf + pos, b,
                                  s->ptr - s->buf_start - len);
            s->level += 4;
            s->ptr = save_ptr;
        }
#endif
        pos += len;
    }
    return 0;
}

static JSValue JS_ReadBigInt(BCReaderState *s)
{
    JSValue obj = JS_UNDEFINED;
    uint32_t len, i, n;
    JSBigInt *p;
    js_limb_t v;
    uint8_t v8;

    if (bc_get_leb128(s, &len))
        goto fail;
    bc_read_trace(s, "len=%" PRId64 "\n", (int64_t)len);
    if (len == 0) {
        /* zero case */
        bc_read_trace(s, "}\n");
        return __JS_NewShortBigInt(s->ctx, 0);
    }
    p = js_bigint_new(s->ctx, (len - 1) / (JS_LIMB_BITS / 8) + 1);
    if (!p)
        goto fail;
    for(i = 0; i < len / (JS_LIMB_BITS / 8); i++) {
        if (bc_get_u32(s, &v))
            goto fail;
        p->tab[i] = v;
    }
    n = len % (JS_LIMB_BITS / 8);
    if (n != 0) {
        int shift;
        v = 0;
        for(i = 0; i < n; i++) {
            if (bc_get_u8(s, &v8))
                goto fail;
            v |= (js_limb_t)v8 << (i * 8);
        }
        shift = JS_LIMB_BITS - n * 8;
        /* extend the sign */
        if (shift != 0) {
            v = (js_slimb_t)(v << shift) >> shift;
        }
        p->tab[p->len - 1] = v;
    }
    bc_read_trace(s, "}\n");
    return JS_CompactBigInt(s->ctx, p);
 fail:
    JS_FreeValue(s->ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectRec(BCReaderState *s);

static int BC_add_object_ref1(BCReaderState *s, JSObject *p)
{
    if (s->allow_reference) {
        if (js_resize_array(s->ctx, (void *)&s->objects,
                            sizeof(s->objects[0]),
                            &s->objects_size, s->objects_count + 1))
            return -1;
        s->objects[s->objects_count++] = p;
    }
    return 0;
}

static int BC_add_object_ref(BCReaderState *s, JSValue obj)
{
    return BC_add_object_ref1(s, JS_VALUE_GET_OBJ(obj));
}

static JSValue JS_ReadFunctionTag(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSFunctionBytecode bc, *b;
    JSValue obj = JS_UNDEFINED;
    uint16_t v16;
    uint8_t v8;
    int idx, i, local_count, has_debug_info;
    int function_size, cpool_offset, byte_code_offset;
    int closure_var_offset, vardefs_offset;

    memset(&bc, 0, sizeof(bc));
    //bc.gc_header.mark = 0;

    if (bc_get_u16(s, &v16))
        goto fail;
    idx = 0;
    bc.has_prototype = bc_get_flags(v16, &idx, 1);
    bc.has_simple_parameter_list = bc_get_flags(v16, &idx, 1);
    bc.is_derived_class_constructor = bc_get_flags(v16, &idx, 1);
    bc.need_home_object = bc_get_flags(v16, &idx, 1);
    bc.func_kind = bc_get_flags(v16, &idx, 2);
    bc.new_target_allowed = bc_get_flags(v16, &idx, 1);
    bc.super_call_allowed = bc_get_flags(v16, &idx, 1);
    bc.super_allowed = bc_get_flags(v16, &idx, 1);
    bc.arguments_allowed = bc_get_flags(v16, &idx, 1);
    bc.backtrace_barrier = bc_get_flags(v16, &idx, 1);
    has_debug_info = bc_get_flags(v16, &idx, 1);
    if (bc_get_u8(s, &v8))
        goto fail;
    bc.is_strict_mode = (v8 > 0);
    if (bc_get_atom(s, &bc.func_name))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.arg_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.var_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.defined_arg_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.stack_size))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.var_ref_count))
        goto fail;
    if (bc_get_leb128_u16(s, &bc.closure_var_count))
        goto fail;
    if (bc_get_leb128_int(s, &bc.cpool_count))
        goto fail;
    if (bc_get_leb128_int(s, &bc.byte_code_len))
        goto fail;
    if (bc_get_leb128_int(s, &local_count))
        goto fail;

    function_size = sizeof(*b);
    cpool_offset = (function_size + 7) & ~7;
    function_size = cpool_offset + bc.cpool_count * sizeof(*bc.cpool);
    vardefs_offset = function_size;
    function_size += local_count * sizeof(*bc.vardefs);
    closure_var_offset = function_size;
    function_size += bc.closure_var_count * sizeof(*bc.closure_var);
    byte_code_offset = function_size;
    function_size += bc.byte_code_len;

    b = js_mallocz(ctx, function_size);
    if (!b)
        goto fail;

    memcpy(b, &bc, sizeof(*b));
    bc.func_name = JS_ATOM_NULL;
    JS_REF_COUNT(b) = 1;
    if (local_count != 0) {
        b->vardefs = (void *)((uint8_t*)b + vardefs_offset);
    }
    if (b->closure_var_count != 0) {
        b->closure_var = (void *)((uint8_t*)b + closure_var_offset);
    }
    if (b->cpool_count != 0) {
        b->cpool = (void *)((uint8_t*)b + cpool_offset);
    }

    add_gc_object(ctx->rt, &b->header, JS_GC_OBJ_TYPE_FUNCTION_BYTECODE);

    obj = JS_MKPTR(JS_TAG_FUNCTION_BYTECODE, b);

#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        if (b->func_name) {
            bc_read_trace(s, "name: ");
            print_atom(s->ctx, b->func_name);
            printf("\n");
        }
    }
#endif
    bc_read_trace(s, "args=%d vars=%d defargs=%d closures=%d cpool=%d\n",
                  b->arg_count, b->var_count, b->defined_arg_count,
                  b->closure_var_count, b->cpool_count);
    bc_read_trace(s, "stack=%d bclen=%d locals=%d\n",
                  b->stack_size, b->byte_code_len, local_count);

    if (local_count != 0) {
        bc_read_trace(s, "vars {\n");
        bc_read_trace(s, "off flags scope name\n");
        for(i = 0; i < local_count; i++) {
            JSVarDef *vd = &b->vardefs[i];
            if (bc_get_atom(s, &vd->var_name))
                goto fail;
            if (bc_get_leb128_int(s, &vd->scope_level))
                goto fail;
            if (bc_get_leb128_int(s, &vd->scope_next))
                goto fail;
            vd->scope_next--;
            if (bc_get_u8(s, &v8))
                goto fail;
            idx = 0;
            vd->var_kind = bc_get_flags(v8, &idx, 4);
            vd->is_const = bc_get_flags(v8, &idx, 1);
            vd->is_lexical = bc_get_flags(v8, &idx, 1);
            vd->is_captured = bc_get_flags(v8, &idx, 1);
            if (vd->is_captured) {
                if (bc_get_leb128_u16(s, &vd->var_ref_idx))
                    goto fail;
            }
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
            if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
                bc_read_trace(s, "%3d  %d%c%c%c %4d  ",
                              i, vd->var_kind,
                              vd->is_const ? 'C' : '.',
                              vd->is_lexical ? 'L' : '.',
                              vd->is_captured ? 'X' : '.',
                              vd->scope_level);
                print_atom(s->ctx, vd->var_name);
                printf("\n");
            }
#endif
        }
        bc_read_trace(s, "}\n");
    }
    if (b->closure_var_count != 0) {
        bc_read_trace(s, "closure vars {\n");
        bc_read_trace(s, "off  flags idx  name\n");
        for(i = 0; i < b->closure_var_count; i++) {
            JSClosureVar *cv = &b->closure_var[i];
            int var_idx, flags;
            if (bc_get_atom(s, &cv->var_name))
                goto fail;
            if (bc_get_leb128_int(s, &var_idx))
                goto fail;
            cv->var_idx = var_idx;
            if (bc_get_leb128_int(s, &flags))
                goto fail;
            idx = 0;
            cv->closure_type = bc_get_flags(flags, &idx, 3);
            cv->is_const = bc_get_flags(flags, &idx, 1);
            cv->is_lexical = bc_get_flags(flags, &idx, 1);
            cv->var_kind = bc_get_flags(flags, &idx, 4);
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
            if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
                bc_read_trace(s, "%3d  %d:%d%c%c %3d  ",
                              i, cv->var_kind, cv->closure_type,
                              cv->is_const ? 'C' : '.',
                              cv->is_lexical ? 'X' : '.',
                              cv->var_idx);
                print_atom(s->ctx, cv->var_name);
                printf("\n");
            }
#endif
        }
        bc_read_trace(s, "}\n");
    }
    if (b->cpool_count != 0) {
        bc_read_trace(s, "cpool {\n");
        for(i = 0; i < b->cpool_count; i++) {
            JSValue val;
            val = JS_ReadObjectRec(s);
            if (JS_IsException(val))
                goto fail;
            b->cpool[i] = val;
        }
        bc_read_trace(s, "}\n");
    }
    {
        bc_read_trace(s, "bytecode {\n");
        if (JS_ReadFunctionBytecode(s, b, byte_code_offset, b->byte_code_len))
            goto fail;
        bc_read_trace(s, "}\n");
    }
    if (!has_debug_info)
        goto nodebug;

    /* read optional debug information */
    bc_read_trace(s, "debug {\n");
    if (bc_get_atom(s, &b->filename))
        goto fail;
    if (bc_get_leb128_int(s, &b->line_num))
        goto fail;
    if (bc_get_leb128_int(s, &b->col_num))
        goto fail;
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        bc_read_trace(s, "filename: ");
        print_atom(s->ctx, b->filename);
        printf(", line: %d, column: %d\n", b->line_num, b->col_num);
    }
#endif
    if (bc_get_leb128_int(s, &b->pc2line_len))
        goto fail;
    if (b->pc2line_len) {
        bc_read_trace(s, "positions: %d bytes\n", b->pc2line_len);
        b->pc2line_buf = js_mallocz(ctx, b->pc2line_len);
        if (!b->pc2line_buf)
            goto fail;
        if (bc_get_buf(s, b->pc2line_buf, b->pc2line_len))
            goto fail;
    }
    if (bc_get_leb128_int(s, &b->source_len))
        goto fail;
    if (b->source_len) {
        bc_read_trace(s, "source: %d bytes\n", b->source_len);
        if (s->ptr_last)
            s->ptr_last += b->source_len;  // omit source code hex dump
        /* b->source is a UTF-8 encoded null terminated C string */
        b->source = js_mallocz(ctx, b->source_len + 1);
        if (!b->source)
            goto fail;
        if (bc_get_buf(s, b->source, b->source_len))
            goto fail;
    }
    bc_read_trace(s, "}\n");

 nodebug:
    b->realm = JS_DupContext(ctx);
    return obj;

 fail:
    JS_FreeAtom(ctx, bc.func_name);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadModule(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue obj;
    JSModuleDef *m = NULL;
    JSAtom module_name;
    int i;
    uint8_t v8;

    if (bc_get_atom(s, &module_name))
        goto fail;
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
    if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
        bc_read_trace(s, "name: ");
        print_atom(s->ctx, module_name);
        printf("\n");
    }
#endif
    m = js_new_module_def(ctx, module_name);
    if (!m)
        goto fail;
    obj = js_dup(JS_MKPTR(JS_TAG_MODULE, m));
    if (bc_get_leb128_int(s, &m->req_module_entries_count))
        goto fail;
    obj = JS_NewModuleValue(ctx, m);
    if (m->req_module_entries_count != 0) {
        m->req_module_entries_size = m->req_module_entries_count;
        m->req_module_entries = js_mallocz(ctx, sizeof(m->req_module_entries[0]) * m->req_module_entries_size);
        if (!m->req_module_entries)
            goto fail;
        for(i = 0; i < m->req_module_entries_count; i++) {
            JSReqModuleEntry *rme = &m->req_module_entries[i];
            JSModuleDef **pm = &rme->module;
            rme->attributes = JS_UNDEFINED;
            if (bc_get_atom(s, &rme->module_name))
                goto fail;
            // Resolves a module either from the cache or by requesting
            // it from the module loader. From cache is not ideal because
            // the module may not be the one it was a time of serialization
            // but directly petitioning the module loader is not correct
            // either because then the same module can get loaded twice.
            // JS_WriteModule() does not serialize modules transitively
            // because that doesn't work for C modules and is also prone
            // to loading the same JS module twice.
            *pm = js_host_resolve_imported_module_atom(s->ctx, m->module_name,
                                                       rme->module_name,
                                                       rme->attributes);
            if (!*pm)
                goto fail;
        }
    }

    if (bc_get_leb128_int(s, &m->export_entries_count))
        goto fail;
    if (m->export_entries_count != 0) {
        m->export_entries_size = m->export_entries_count;
        m->export_entries = js_mallocz(ctx, sizeof(m->export_entries[0]) * m->export_entries_size);
        if (!m->export_entries)
            goto fail;
        for(i = 0; i < m->export_entries_count; i++) {
            JSExportEntry *me = &m->export_entries[i];
            if (bc_get_u8(s, &v8))
                goto fail;
            me->export_type = v8;
            if (me->export_type == JS_EXPORT_TYPE_LOCAL) {
                if (bc_get_leb128_int(s, &me->u.local.var_idx))
                    goto fail;
            } else {
                if (bc_get_leb128_int(s, &me->u.req_module_idx))
                    goto fail;
                if (bc_get_atom(s, &me->local_name))
                    goto fail;
            }
            if (bc_get_atom(s, &me->export_name))
                goto fail;
        }
    }

    if (bc_get_leb128_int(s, &m->star_export_entries_count))
        goto fail;
    if (m->star_export_entries_count != 0) {
        m->star_export_entries_size = m->star_export_entries_count;
        m->star_export_entries = js_mallocz(ctx, sizeof(m->star_export_entries[0]) * m->star_export_entries_size);
        if (!m->star_export_entries)
            goto fail;
        for(i = 0; i < m->star_export_entries_count; i++) {
            JSStarExportEntry *se = &m->star_export_entries[i];
            if (bc_get_leb128_int(s, &se->req_module_idx))
                goto fail;
        }
    }

    if (bc_get_leb128_int(s, &m->import_entries_count))
        goto fail;
    if (m->import_entries_count != 0) {
        m->import_entries_size = m->import_entries_count;
        m->import_entries = js_mallocz(ctx, sizeof(m->import_entries[0]) * m->import_entries_size);
        if (!m->import_entries)
            goto fail;
        for(i = 0; i < m->import_entries_count; i++) {
            JSImportEntry *mi = &m->import_entries[i];
            if (bc_get_leb128_int(s, &mi->var_idx))
                goto fail;
            if (bc_get_atom(s, &mi->import_name))
                goto fail;
            if (bc_get_leb128_int(s, &mi->req_module_idx))
                goto fail;
        }
    }

    if (bc_get_u8(s, &v8))
        goto fail;
    m->has_tla = (v8 != 0);

    m->func_obj = JS_ReadObjectRec(s);
    if (JS_IsException(m->func_obj))
        goto fail;
    return obj;
 fail:
    if (m) {
        js_free_module_def(ctx, m);
    }
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectTag(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue obj;
    uint32_t prop_count, i;
    JSAtom atom;
    JSValue val;
    int ret;

    obj = JS_NewObject(ctx);
    if (BC_add_object_ref(s, obj))
        goto fail;
    if (bc_get_leb128(s, &prop_count))
        goto fail;
    for(i = 0; i < prop_count; i++) {
        if (bc_get_atom(s, &atom))
            goto fail;
#ifdef ENABLE_DUMPS // JS_DUMP_READ_OBJECT
        if (check_dump_flag(s->ctx->rt, JS_DUMP_READ_OBJECT)) {
            bc_read_trace(s, "propname: ");
            print_atom(s->ctx, atom);
            printf("\n");
        }
#endif
        val = JS_ReadObjectRec(s);
        if (JS_IsException(val)) {
            JS_FreeAtom(ctx, atom);
            goto fail;
        }
        ret = JS_DefinePropertyValue(ctx, obj, atom, val, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, atom);
        if (ret < 0)
            goto fail;
    }
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadArray(BCReaderState *s, int tag)
{
    JSContext *ctx = s->ctx;
    JSValue obj;
    uint32_t len, i;
    JSValue val;
    int ret, prop_flags;
    bool is_template;

    obj = JS_NewArray(ctx);
    if (BC_add_object_ref(s, obj))
        goto fail;
    is_template = (tag == BC_TAG_TEMPLATE_OBJECT);
    if (bc_get_leb128(s, &len))
        goto fail;
    for(i = 0; i < len; i++) {
        val = JS_ReadObjectRec(s);
        if (JS_IsException(val))
            goto fail;
        if (is_template)
            prop_flags = JS_PROP_ENUMERABLE;
        else
            prop_flags = JS_PROP_C_W_E;
        ret = JS_DefinePropertyValueUint32(ctx, obj, i, val,
                                           prop_flags);
        if (ret < 0)
            goto fail;
    }
    if (is_template) {
        val = JS_ReadObjectRec(s);
        if (JS_IsException(val))
            goto fail;
        if (!JS_IsUndefined(val)) {
            ret = JS_DefinePropertyValue(ctx, obj, JS_ATOM_raw, val, 0);
            if (ret < 0)
                goto fail;
        }
        JS_PreventExtensions(ctx, obj);
    }
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadTypedArray(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue obj = JS_UNDEFINED, array_buffer = JS_UNDEFINED;
    uint8_t array_tag;
    JSValueConst args[3];
    uint32_t offset, len, idx;

    if (bc_get_u8(s, &array_tag))
        return JS_EXCEPTION;
    if (array_tag >= JS_TYPED_ARRAY_COUNT)
        return JS_ThrowTypeError(ctx, "invalid typed array");
    if (bc_get_leb128(s, &len))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &offset))
        return JS_EXCEPTION;
    /* XXX: this hack could be avoided if the typed array could be
       created before the array buffer */
    idx = s->objects_count;
    if (BC_add_object_ref1(s, NULL))
        goto fail;
    array_buffer = JS_ReadObjectRec(s);
    if (JS_IsException(array_buffer))
        return JS_EXCEPTION;
    if (!js_get_array_buffer(ctx, array_buffer)) {
        JS_FreeValue(ctx, array_buffer);
        return JS_EXCEPTION;
    }
    args[0] = array_buffer;
    args[1] = js_int64(offset);
    args[2] = js_int64(len);
    obj = js_typed_array_constructor(ctx, JS_UNDEFINED,
                                     3, args,
                                     JS_CLASS_UINT8C_ARRAY + array_tag);
    if (JS_IsException(obj))
        goto fail;
    if (s->allow_reference) {
        s->objects[idx] = JS_VALUE_GET_OBJ(obj);
    }
    JS_FreeValue(ctx, array_buffer);
    return obj;
 fail:
    JS_FreeValue(ctx, array_buffer);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadArrayBuffer(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    uint32_t byte_length, max_byte_length;
    uint64_t max_byte_length_u64, *pmax_byte_length = NULL;
    JSValue obj;

    if (bc_get_leb128(s, &byte_length))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &max_byte_length))
        return JS_EXCEPTION;
    if (max_byte_length < byte_length)
        return JS_ThrowTypeError(ctx, "invalid array buffer");
    if (max_byte_length != UINT32_MAX) {
        max_byte_length_u64 = max_byte_length;
        pmax_byte_length = &max_byte_length_u64;
    }
    if (unlikely(s->buf_end - s->ptr < byte_length)) {
        bc_read_error_end(s);
        return JS_EXCEPTION;
    }
    // makes a copy of the input
    obj = js_array_buffer_constructor3(ctx, JS_UNDEFINED,
                                       byte_length, pmax_byte_length,
                                       JS_CLASS_ARRAY_BUFFER,
                                       (uint8_t*)s->ptr,
                                       js_array_buffer_realloc, NULL,
                                       /*alloc_flag*/true);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    s->ptr += byte_length;
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadSharedArrayBuffer(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    uint32_t byte_length, max_byte_length;
    uint64_t max_byte_length_u64, *pmax_byte_length = NULL;
    uint8_t *data_ptr;
    JSValue obj;
    uint64_t u64;

    if (bc_get_leb128(s, &byte_length))
        return JS_EXCEPTION;
    if (bc_get_leb128(s, &max_byte_length))
        return JS_EXCEPTION;
    if (max_byte_length < byte_length)
        return JS_ThrowTypeError(ctx, "invalid array buffer");
    if (max_byte_length != UINT32_MAX) {
        max_byte_length_u64 = max_byte_length;
        pmax_byte_length = &max_byte_length_u64;
    }
    if (bc_get_u64(s, &u64))
        return JS_EXCEPTION;
    data_ptr = (uint8_t *)(uintptr_t)u64;
    if (js_resize_array(s->ctx, (void **)&s->sab_tab, sizeof(s->sab_tab[0]),
                        &s->sab_tab_size, s->sab_tab_len + 1))
        return JS_EXCEPTION;
    /* keep the SAB pointer so that the user can clone it or free it */
    s->sab_tab[s->sab_tab_len++] = data_ptr;
    /* the SharedArrayBuffer is cloned */
    obj = js_array_buffer_constructor3(ctx, JS_UNDEFINED,
                                       byte_length, pmax_byte_length,
                                       JS_CLASS_SHARED_ARRAY_BUFFER,
                                       data_ptr,
                                       NULL, NULL, false);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    return obj;
 fail:
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadRegExp(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSString *pattern;
    JSString *bc;

    pattern = JS_ReadString(s);
    if (!pattern)
        return JS_EXCEPTION;

    bc = JS_ReadString(s);
    if (!bc) {
        js_free_string(ctx->rt, pattern);
        return JS_EXCEPTION;
    }

    if (bc->is_wide_char ||
        lre_check_bytecode(str8(bc), bc->len) != 0) {
        js_free_string(ctx->rt, pattern);
        js_free_string(ctx->rt, bc);
        return JS_ThrowInternalError(ctx, "bad regexp bytecode");
    }

    return js_regexp_constructor_internal(ctx, JS_UNDEFINED,
                                          JS_MKPTR(JS_TAG_STRING, pattern),
                                          JS_MKPTR(JS_TAG_STRING, bc));
}

static JSValue JS_ReadDate(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue val, obj = JS_UNDEFINED;

    val = JS_ReadObjectRec(s);
    if (JS_IsException(val))
        goto fail;
    if (!JS_IsNumber(val)) {
        JS_ThrowTypeError(ctx, "Number tag expected for date");
        goto fail;
    }
    obj = JS_NewObjectProtoClass(ctx, ctx->class_proto[JS_CLASS_DATE],
                                 JS_CLASS_DATE);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    JS_SetObjectData(ctx, obj, val);
    return obj;
 fail:
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadObjectValue(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    JSValue val, obj = JS_UNDEFINED;

    val = JS_ReadObjectRec(s);
    if (JS_IsException(val))
        goto fail;
    obj = JS_ToObject(ctx, val);
    if (JS_IsException(obj))
        goto fail;
    if (BC_add_object_ref(s, obj))
        goto fail;
    JS_FreeValue(ctx, val);
    return obj;
 fail:
    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue JS_ReadMap(BCReaderState *s);
static JSValue JS_ReadSet(BCReaderState *s);

static JSValue JS_ReadObjectRec(BCReaderState *s)
{
    JSContext *ctx = s->ctx;
    uint8_t tag;
    JSValue obj = JS_UNDEFINED;

    if (js_check_stack_overflow(ctx->rt, 0))
        return JS_ThrowStackOverflow(ctx);

    if (bc_get_u8(s, &tag))
        return JS_EXCEPTION;

    bc_read_trace(s, "%s {\n", bc_tag_name(tag));

    switch(tag) {
    case BC_TAG_NULL:
        obj = JS_NULL;
        break;
    case BC_TAG_UNDEFINED:
        obj = JS_UNDEFINED;
        break;
    case BC_TAG_BOOL_FALSE:
    case BC_TAG_BOOL_TRUE:
        obj = js_bool(tag - BC_TAG_BOOL_FALSE);
        break;
    case BC_TAG_INT32:
        {
            int32_t val;
            if (bc_get_sleb128(s, &val))
                return JS_EXCEPTION;
            bc_read_trace(s, "%d\n", val);
            obj = js_int32(val);
        }
        break;
    case BC_TAG_FLOAT64:
        {
            JSFloat64Union u;
            if (bc_get_u64(s, &u.u64))
                return JS_EXCEPTION;
            bc_read_trace(s, "%g\n", u.d);
            obj = js_float64(u.d);
        }
        break;
    case BC_TAG_STRING:
        {
            JSString *p;
            p = JS_ReadString(s);
            if (!p)
                return JS_EXCEPTION;
            obj = JS_MKPTR(JS_TAG_STRING, p);
        }
        break;
    case BC_TAG_FUNCTION_BYTECODE:
        if (!s->allow_bytecode)
            goto no_allow_bytecode;
        obj = JS_ReadFunctionTag(s);
        break;
    case BC_TAG_MODULE:
        if (!s->allow_bytecode) {
        no_allow_bytecode:
            return JS_ThrowSyntaxError(ctx, "no bytecode allowed");
        }
        obj = JS_ReadModule(s);
        break;
    case BC_TAG_OBJECT:
        obj = JS_ReadObjectTag(s);
        break;
    case BC_TAG_ARRAY:
    case BC_TAG_TEMPLATE_OBJECT:
        obj = JS_ReadArray(s, tag);
        break;
    case BC_TAG_TYPED_ARRAY:
        obj = JS_ReadTypedArray(s);
        break;
    case BC_TAG_ARRAY_BUFFER:
        obj = JS_ReadArrayBuffer(s);
        break;
    case BC_TAG_SHARED_ARRAY_BUFFER:
        if (!s->allow_sab || !ctx->rt->sab_funcs.sab_dup)
            goto invalid_tag;
        obj = JS_ReadSharedArrayBuffer(s);
        break;
    case BC_TAG_REGEXP:
        obj = JS_ReadRegExp(s);
        break;
    case BC_TAG_DATE:
        obj = JS_ReadDate(s);
        break;
    case BC_TAG_OBJECT_VALUE:
        obj = JS_ReadObjectValue(s);
        break;
    case BC_TAG_BIG_INT:
        obj = JS_ReadBigInt(s);
        break;
    case BC_TAG_OBJECT_REFERENCE:
        {
            uint32_t val;
            if (!s->allow_reference)
                return JS_ThrowSyntaxError(ctx, "object references are not allowed");
            if (bc_get_leb128(s, &val))
                return JS_EXCEPTION;
            bc_read_trace(s, "%u\n", val);
            if (val >= s->objects_count || !s->objects[val]) {
                return JS_ThrowSyntaxError(ctx, "invalid object reference (%u >= %u)",
                                           val, s->objects_count);
            }
            obj = js_dup(JS_MKPTR(JS_TAG_OBJECT, s->objects[val]));
        }
        break;
    case BC_TAG_MAP:
        obj = JS_ReadMap(s);
        break;
    case BC_TAG_SET:
        obj = JS_ReadSet(s);
        break;
    case BC_TAG_SYMBOL:
        {
            JSAtom atom;
            if (bc_get_atom(s, &atom))
                return JS_EXCEPTION;
            if (__JS_AtomIsConst(atom)) {
                obj = JS_AtomToValue(s->ctx, atom);
            } else {
                JSAtomStruct *p = s->ctx->rt->atom_array[atom];
                obj = JS_NewSymbolFromAtom(s->ctx, atom, p->atom_type);
            }
        }
        break;
    default:
    invalid_tag:
        return JS_ThrowSyntaxError(ctx, "invalid tag (tag=%d pos=%u)",
                                   tag, (unsigned int)(s->ptr - s->buf_start));
    }
    bc_read_trace(s, "}\n");
    return obj;
}

static int JS_ReadObjectAtoms(BCReaderState *s)
{
    uint8_t v8, type;
    JSString *p;
    uint32_t h;
    int i;
    JSAtom atom;

    if (bc_get_u8(s, &v8))
        return -1;
    if (v8 != BC_VERSION) {
        JS_ThrowSyntaxError(s->ctx, "invalid version (%d expected=%d)",
                            v8, BC_VERSION);
        return -1;
    }
    if (bc_get_u32(s, &h))
        return -1;
    // allow UINT32_MAX as an escape hatch, otherwise updating
    // the test corpus in tests/test_bjson.js gets too tedious
    if (h != UINT32_MAX && h != bc_csum(s->ptr, s->buf_end - s->ptr)) {
        JS_ThrowSyntaxError(s->ctx, "checksum error");
        return -1;
    }
    if (bc_get_leb128(s, &s->idx_to_atom_count))
        return -1;
    if (s->idx_to_atom_count > 1000*1000) {
        JS_ThrowInternalError(s->ctx, "unreasonable atom count: %u",
                              s->idx_to_atom_count);
        return -1;
    }

    bc_read_trace(s, "%u atom indexes {\n", s->idx_to_atom_count);

    if (s->idx_to_atom_count != 0) {
        s->idx_to_atom = js_mallocz(s->ctx, s->idx_to_atom_count *
                                    sizeof(s->idx_to_atom[0]));
        if (!s->idx_to_atom)
            return s->error_state = -1;
    }
    for(i = 0; i < s->idx_to_atom_count; i++) {
        if (bc_get_u8(s, &type)) {
            return -1;
        }
        if (type == 0) {
            if (bc_get_u32(s, &atom))
                return -1;
            if (!__JS_AtomIsConst(atom)) {
                JS_ThrowInternalError(s->ctx, "out of range atom");
                return -1;
            }
        } else {
            if (type < JS_ATOM_TYPE_STRING || type >= JS_ATOM_TYPE_PRIVATE) {
                JS_ThrowInternalError(s->ctx, "invalid symbol type %d", type);
                return -1;
            }
            p = JS_ReadString(s);
            if (!p)
                return -1;
            atom = __JS_NewAtom(s->ctx->rt, p, type);
        }
        if (atom == JS_ATOM_NULL)
            return s->error_state = -1;
        s->idx_to_atom[i] = atom;
    }
    bc_read_trace(s, "}\n");
    return 0;
}

static void bc_reader_free(BCReaderState *s)
{
    int i;
    if (s->idx_to_atom) {
        for(i = 0; i < s->idx_to_atom_count; i++) {
            JS_FreeAtom(s->ctx, s->idx_to_atom[i]);
        }
        js_free(s->ctx, s->idx_to_atom);
    }
    js_free(s->ctx, s->objects);
}

JSValue JS_ReadObject2(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                       int flags, JSSABTab *psab_tab)
{
    BCReaderState ss, *s = &ss;
    JSValue obj;

    ctx->binary_object_count += 1;
    ctx->binary_object_size += buf_len;

    memset(s, 0, sizeof(*s));
    s->ctx = ctx;
    s->buf_start = buf;
    s->buf_end = buf + buf_len;
    s->ptr = buf;
    s->allow_bytecode = ((flags & JS_READ_OBJ_BYTECODE) != 0);
    s->allow_sab = ((flags & JS_READ_OBJ_SAB) != 0);
    s->allow_reference = ((flags & JS_READ_OBJ_REFERENCE) != 0);
    if (s->allow_bytecode)
        s->first_atom = JS_ATOM_END;
    else
        s->first_atom = 1;
    if (JS_ReadObjectAtoms(s)) {
        obj = JS_EXCEPTION;
    } else {
        obj = JS_ReadObjectRec(s);
    }
    if (psab_tab) {
        psab_tab->tab = s->sab_tab;
        psab_tab->len = s->sab_tab_len;
    } else {
        js_free(ctx, s->sab_tab);
    }
    bc_reader_free(s);
    return obj;
}

JSValue JS_ReadObject(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                      int flags)
{
    return JS_ReadObject2(ctx, buf, buf_len, flags, NULL);
}

