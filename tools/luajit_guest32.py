#!/usr/bin/env python3
"""Patches LuaJIT 2.1 so its FFI works on a 32-bit x86 guest's memory from a 64-bit host.

    tools/luajit_guest32.py third_party/luajit

Built with -DLUAJIT_GUEST32 (and -DLUAJIT_DISABLE_JIT), the FFI behaves like the
game's own 32-bit Windows LuaJIT:
  - pointers and references are 4 bytes and hold guest addresses; reading one
    gives g_mem + address (0 <-> NULL), writing one stores address - g_mem;
  - long, size_t, intptr_t, ssize_t are 32 bit, wchar_t is 16 bit (Win32);
  - calling a C function (cdata or ffi.C.name) calls guest code through the
    runtime's lj_guest_ffi_call(), honouring cdecl/stdcall/thiscall/fastcall;
  - ffi.C.name resolves through lj_guest_resolve() (the emulated Win32 API);
  - Lua functions cannot become C callbacks (the scripts never do that).
The Lua allocator must hand out guest memory (lua_newstate with an allocator on
the guest heap), so cdata and strings have guest addresses too.
Idempotent: patched files carry a marker."""
import sys, os, re

root = os.path.join(sys.argv[1], 'src')
MARK = '/* LUAJIT_GUEST32 patched */'


def patch(name, pairs):
    p = os.path.join(root, name)
    s = open(p).read()
    if MARK in s:
        return
    for old, new in pairs:
        if old not in s:
            sys.exit('%s: pattern not found:\n%s' % (name, old))
        s = s.replace(old, new, 1)
    open(p, 'w').write(MARK + '\n' + s)


patch('lj_ctype.h', [
    ('''#if LJ_64
#define CTSIZE_PTR	8
#define CTALIGN_PTR	CTALIGN(3)''', '''#if LJ_64 && !defined(LUAJIT_GUEST32)
#define CTSIZE_PTR	8
#define CTALIGN_PTR	CTALIGN(3)'''),
    ('''#define CTF_LONG_IF8		(CTF_LONG * (sizeof(long) == 8))''',
     '''#ifdef LUAJIT_GUEST32
#define CTF_LONG_IF8		0
#else
#define CTF_LONG_IF8		(CTF_LONG * (sizeof(long) == 8))
#endif'''),
    ('''/* Target-dependent type IDs. */
#if LJ_64''', '''/* Target-dependent type IDs. */
#if LJ_64 && !defined(LUAJIT_GUEST32)'''),
    ('''#if LJ_ABI_WIN
#define CTID_WCHAR	CTID_UINT16''', '''#if LJ_ABI_WIN || defined(LUAJIT_GUEST32)
#define CTID_WCHAR	CTID_UINT16'''),
])

patch('lj_cdata.h', [
    ('''/* Get C data pointer. */
static LJ_AINLINE void *cdata_getptr(void *p, CTSize sz)
{''', '''#ifdef LUAJIT_GUEST32
/* Guest pointers: 32-bit addresses into the guest address space at g_mem. */
extern unsigned char *g_mem;
#define lj_guest_g2h(a)	((a) ? (void *)(g_mem + (uint32_t)(a)) : NULL)
#define lj_guest_h2g(p)	((p) ? (uint32_t)((const unsigned char *)(p) - g_mem) : 0u)
LJ_FUNC void *lj_guest_resolve(const char *name);
LJ_FUNC int lj_guest_ffi_call(uint32_t target, const uint32_t *stack, int nstack, int nreg,
			      uint32_t reg0, uint32_t reg1, uint32_t *eax, uint32_t *edx,
			      double *st0, int fpret);

static LJ_AINLINE void *cdata_getptr(void *p, CTSize sz)
{
  uint32_t v = sz == 4 ? *(uint32_t *)p : (uint32_t)*(uint64_t *)p;
  return lj_guest_g2h(v);
}

static LJ_AINLINE void cdata_setptr(void *p, CTSize sz, const void *v)
{
  uint32_t g = lj_guest_h2g(v);
  if (sz == 4) *(uint32_t *)p = g; else *(uint64_t *)p = g;
}
#else
/* Get C data pointer. */
static LJ_AINLINE void *cdata_getptr(void *p, CTSize sz)
{'''),
    ('''    *(void **)p = (void *)v;
  }
}''', '''    *(void **)p = (void *)v;
  }
}
#endif'''),
])

patch('lj_cdata.c', [
    ('  *(const void **)cdataptr(cd) = p;', '  cdata_setptr(cdataptr(cd), CTSIZE_PTR, p);'),
    ('    p = *(uint8_t **)p;\n    ct = ctype_child(cts, ct);', '    p = (uint8_t *)cdata_getptr(p, CTSIZE_PTR);\n    ct = ctype_child(cts, ct);'),
    ('    sp = *(uint8_t **)sp;', '    sp = (uint8_t *)cdata_getptr(sp, CTSIZE_PTR);'),
    ('    dp = *(uint8_t **)dp;', '    dp = (uint8_t *)cdata_getptr(dp, CTSIZE_PTR);'),
])

patch('lj_cconv.c', [
    ('''  case CCX(I, A):
    if (!(flags & CCF_CAST)) goto err_conv;
    sinfo = CTINFO(CT_NUM, CTF_UNSIGNED);
    ssize = CTSIZE_PTR;
    tmpptr = sp;
    sp = (uint8_t *)&tmpptr;
    goto conv_I_I;''', '''  case CCX(I, A):
    if (!(flags & CCF_CAST)) goto err_conv;
    sinfo = CTINFO(CT_NUM, CTF_UNSIGNED);
    ssize = CTSIZE_PTR;
#ifdef LUAJIT_GUEST32
    tmpg = lj_guest_h2g(sp);  /* the array's guest address */
    sp = (uint8_t *)&tmpg;
#else
    tmpptr = sp;
    sp = (uint8_t *)&tmpptr;
#endif
    goto conv_I_I;'''),
    ('''      sp = *(void **)sp;
      sid = ctype_cid(s->info);
    }
    s = ctype_raw(cts, sid);
    if (ctype_isfunc(s->info)) {''', '''      sp = (uint8_t *)cdata_getptr(sp, CTSIZE_PTR);
      sid = ctype_cid(s->info);
    }
    s = ctype_raw(cts, sid);
    if (ctype_isfunc(s->info)) {'''),
    ('''    if (ud->udtype == UDTYPE_IO_FILE)
      tmpptr = *(void **)tmpptr;''', '''    if (ud->udtype == UDTYPE_IO_FILE)
      tmpptr = *(void **)tmpptr;  /* host FILE*: not a guest pointer */'''),
    ('''  } else if (tvislightud(o)) {
    tmpptr = lightudV(cts->g, o);''', '''  } else if (tvislightud(o)) {
    tmpptr = lightudV(cts->g, o);
#ifdef LUAJIT_GUEST32
    *(uint64_t *)&tmpptr = lj_guest_h2g(tmpptr);  /* read as a 4-byte guest pointer */
#endif'''),
    ('''    else if (ud->udtype == UDTYPE_BUFFER)
      tmpptr = ((SBufExt *)tmpptr)->r;''', '''    else if (ud->udtype == UDTYPE_BUFFER)
      tmpptr = ((SBufExt *)tmpptr)->r;
#ifdef LUAJIT_GUEST32
    if (ud->udtype != UDTYPE_IO_FILE) *(uint64_t *)&tmpptr = lj_guest_h2g(tmpptr);
#endif'''),
    ('''    if (p) {
      *(void **)dp = p;
      return;
    }''', '''    if (p) {
      cdata_setptr(dp, CTSIZE_PTR, p);
      return;
    }'''),
])
# a local for the CCX(I, A) case
p = os.path.join(root, 'lj_cconv.c')
s = open(p).read()
if 'uint32_t tmpg;' not in s:
    s = s.replace('''void lj_cconv_ct_ct(CTState *cts, CType *d, CType *s,
		    uint8_t *dp, uint8_t *sp, CTInfo flags)
{''', '''void lj_cconv_ct_ct(CTState *cts, CType *d, CType *s,
		    uint8_t *dp, uint8_t *sp, CTInfo flags)
{
#ifdef LUAJIT_GUEST32
  uint32_t tmpg;
#endif''', 1)
    open(p, 'w').write(s)

patch('lj_carith.c', [
    ('	p = (uint8_t *)*(void **)p;', '	p = (uint8_t *)cdata_getptr(p, CTSIZE_PTR);'),
    ('  *(uint8_t **)cdataptr(cd) = pp;', '  cdata_setptr(cdataptr(cd), CTSIZE_PTR, pp);'),
    ('''    if (ctype_isref(s->info)) {
      sp = *(void **)sp;
      sid = ctype_cid(s->info);
    }
    s = ctype_raw(cts, sid);
    if (ctype_isenum(s->info)) s = ctype_child(cts, s);
    if ((s->info & (CTMASK_NUM|CTF_BOOL|CTF_FP|CTF_UNSIGNED)) ==
	CTINFO(CT_NUM, CTF_UNSIGNED) && s->size == 8)
      *id = CTID_UINT64;''', '''    if (ctype_isref(s->info)) {
      sp = (uint8_t *)cdata_getptr(sp, CTSIZE_PTR);
      sid = ctype_cid(s->info);
    }
    s = ctype_raw(cts, sid);
    if (ctype_isenum(s->info)) s = ctype_child(cts, s);
    if ((s->info & (CTMASK_NUM|CTF_BOOL|CTF_FP|CTF_UNSIGNED)) ==
	CTINFO(CT_NUM, CTF_UNSIGNED) && s->size == 8)
      *id = CTID_UINT64;'''),
    ('''    if (ctype_isref(s->info)) {
      sp = *(void **)sp;
      sid = ctype_cid(s->info);
    }''', '''    if (ctype_isref(s->info)) {
      sp = (uint8_t *)cdata_getptr(sp, CTSIZE_PTR);
      sid = ctype_cid(s->info);
    }'''),
])

patch('lib_ffi.c', [
    ('''    if (ctype_isref(ct->info)) {
      p = *(void **)p;
      ct = ctype_rawchild(cts, ct);
    }''', '''    if (ctype_isref(ct->info)) {
      p = cdata_getptr(p, CTSIZE_PTR);
      ct = ctype_rawchild(cts, ct);
    }'''),
    ('''    } else if (ctype_isfunc(ct->info)) {
      p = *(void **)p;
    } else if (ctype_isenum(ct->info)) {''', '''    } else if (ctype_isfunc(ct->info)) {
      p = cdata_getptr(p, CTSIZE_PTR);
    } else if (ctype_isenum(ct->info)) {'''),
    ('''  lj_strfmt_pushf(L, msg, strdata(lj_ctype_repr(L, id, NULL)), p);''', '''#ifdef LUAJIT_GUEST32
  if (msg[strlen(msg)-1] == 'p') p = (void *)(uintptr_t)lj_guest_h2g(p);  /* show guest addresses */
#endif
  lj_strfmt_pushf(L, msg, strdata(lj_ctype_repr(L, id, NULL)), p);'''),
    ('      void *sp = *(void **)cdataptr(cd);', '      void *sp = cdata_getptr(cdataptr(cd), CTSIZE_PTR);'),
    ('''  lj_cconv_ct_tv(cts, ctype_get(cts, id), (uint8_t *)&p, o, CCF_ARG(narg));
  return p;''', '''  lj_cconv_ct_tv(cts, ctype_get(cts, id), (uint8_t *)&p, o, CCF_ARG(narg));
#ifdef LUAJIT_GUEST32
  p = cdata_getptr(&p, CTSIZE_PTR);  /* converted to a guest pointer */
#endif
  return p;'''),
    ('''    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&p, o,
		   CCF_ARG(1));
  } else {
    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CCHAR), (uint8_t *)&p, o,
		   CCF_ARG(1));
    len = strlen(p);''', '''    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&p, o,
		   CCF_ARG(1));
#ifdef LUAJIT_GUEST32
    p = (const char *)cdata_getptr(&p, CTSIZE_PTR);
#endif
  } else {
    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CCHAR), (uint8_t *)&p, o,
		   CCF_ARG(1));
#ifdef LUAJIT_GUEST32
    p = (const char *)cdata_getptr(&p, CTSIZE_PTR);
#endif
    len = strlen(p);'''),
    ('	lj_cconv_ct_tv(cts, d, *(void **)cdataptr(cd), o, 0);', '	lj_cconv_ct_tv(cts, d, cdata_getptr(cdataptr(cd), CTSIZE_PTR), o, 0);'),
])

patch('lib_buffer.c', [
    ('''    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&p,
		   L->base+1, CCF_ARG(2));
    len =''', '''    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&p,
		   L->base+1, CCF_ARG(2));
#ifdef LUAJIT_GUEST32
    p = cdata_getptr(&p, CTSIZE_PTR);
#endif
    len ='''),
    ('''    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&p,
		   L->base+1, CCF_ARG(2));
  } else {''', '''    lj_cconv_ct_tv(cts, ctype_get(cts, CTID_P_CVOID), (uint8_t *)&p,
		   L->base+1, CCF_ARG(2));
#ifdef LUAJIT_GUEST32
    p = cdata_getptr(&p, CTSIZE_PTR);
#endif
  } else {'''),
])

patch('lj_clib.c', [
    ('      void *p = clib_getsym(cl, sym);', '''#ifdef LUAJIT_GUEST32
      void *p = lj_guest_resolve(sym);  /* the emulated Win32 API */
      (void)cl;
#else
      void *p = clib_getsym(cl, sym);
#endif'''),
    ('      *(void **)cdataptr(cd) = p;', '      cdata_setptr(cdataptr(cd), CTSIZE_PTR, p);'),
])

patch('lj_ccallback.c', [
    ('''void *lj_ccallback_new(CTState *cts, CType *ct, GCfunc *fn)
{''', '''void *lj_ccallback_new(CTState *cts, CType *ct, GCfunc *fn)
{
#ifdef LUAJIT_GUEST32
  (void)cts; (void)ct; (void)fn;
  return NULL;  /* guest code can't call Lua functions through C pointers */
#endif'''),
])

# x86 calling conventions (cdecl/thiscall/fastcall/stdcall attributes) are parsed for the guest
p = os.path.join(root, 'lj_cparse.c')
t = open(p).read()
if MARK not in t:
    t = t.replace('#if LJ_TARGET_X86\n', '#if LJ_TARGET_X86 || defined(LUAJIT_GUEST32)\n')
    open(p, 'w').write(t)

patch('lj_cparse.c', [
    ('''      info |= CTF_LONG;
      sz = sizeof(long);''', '''      info |= CTF_LONG;
#ifdef LUAJIT_GUEST32
      sz = 4;  /* Win32: long is 32 bit */
#else
      sz = sizeof(long);
#endif'''),
])

# The call path: replace the native call (lj_vm_ffi_call) with a guest call.
p = os.path.join(root, 'lj_ccall.c')
s = open(p).read()
if MARK not in s:
    guest = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'luajit_guest32_ccall.c')).read()
    i = s.index('#if LJ_HASFFI')
    s = MARK + '\n' + s[:i] + '#if LJ_HASFFI && defined(LUAJIT_GUEST32)\n' + guest + '\n#elif LJ_HASFFI' + s[i + len('#if LJ_HASFFI'):]
    open(p, 'w').write(s)
print('LuaJIT patched for LUAJIT_GUEST32')
