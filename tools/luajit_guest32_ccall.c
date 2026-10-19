/* LUAJIT_GUEST32: FFI calls go to 32-bit x86 guest code (Win32 ABI).
** Inserted into lj_ccall.c by tools/luajit_guest32.py. */

#include "lj_gc.h"
#include "lj_err.h"
#include "lj_tab.h"
#include "lj_ctype.h"
#include "lj_cconv.h"
#include "lj_cdata.h"
#include "lj_ccall.h"

/* Infer the C type of a vararg (same rules as the native x86 ABI). */
CTypeID lj_ccall_ctid_vararg(CTState *cts, cTValue *o)
{
  if (tvisnumber(o)) {
    return CTID_DOUBLE;
  } else if (tviscdata(o)) {
    CTypeID id = cdataV(o)->ctypeid;
    CType *s = ctype_get(cts, id);
    if (ctype_isrefarray(s->info)) {
      return lj_ctype_intern(cts,
	       CTINFO(CT_PTR, CTALIGN_PTR|ctype_cid(s->info)), CTSIZE_PTR);
    } else if (ctype_isstruct(s->info) || ctype_isfunc(s->info)) {
      return lj_ctype_intern(cts, CTINFO(CT_PTR, CTALIGN_PTR|id), CTSIZE_PTR);
    } else if (ctype_isfp(s->info) && s->size == sizeof(float)) {
      return CTID_DOUBLE;
    } else {
      return id;
    }
  } else if (tvisstr(o)) {
    return CTID_P_CCHAR;
  } else if (tvisbool(o)) {
    return CTID_BOOL;
  } else {
    return CTID_P_VOID;
  }
}

#define GUEST_MAXSTACK 64

/* Call a C function: marshal the Lua arguments to the x86 stack (and ECX/EDX for
** thiscall/fastcall), run the guest function, convert EAX/EDX/ST0 back. */
int lj_ccall_func(lua_State *L, GCcdata *cd)
{
  CTState *cts = ctype_cts(L);
  CType *ct = ctype_raw(cts, cd->ctypeid);
  CTSize sz = CTSIZE_PTR;
  if (ctype_isptr(ct->info)) {
    sz = ct->size;
    ct = ctype_rawchild(cts, ct);
  }
  if (ctype_isfunc(ct->info)) {
    CTypeID id = ctype_typeid(cts, ct);
    CTInfo info = ct->info;
    uint32_t target = lj_guest_h2g(cdata_getptr(cdataptr(cd), sz));
    uint32_t stack[GUEST_MAXSTACK], reg[2] = {0, 0};
    int nstack = 0, nreg = 0, maxreg = 0, gcsteps = 0, ret;
    TValue *o, *top = L->top;
    CTypeID fid;
    CType *ctr;
    MSize narg;
    uint32_t eax = 0, edx = 0;
    double st0 = 0;
    void *retp = NULL;  /* struct result returned through a hidden pointer */
    switch (ctype_cconv(info)) {
    case CTCC_FASTCALL: maxreg = 2; break;
    case CTCC_THISCALL: maxreg = 1; break;
    default: maxreg = 0; break;
    }
    ctr = ctype_rawchild(cts, ct);
    if (ctype_isstruct(ctr->info) || ctype_iscomplex(ctr->info)) {
      /* Preallocate the result object and anchor it after the arguments. */
      GCcdata *rcd = lj_cdata_new(cts, ctype_cid(info), ctr->size);
      setcdataV(L, L->top++, rcd);
      if (ctr->size > 8) {  /* MSVC returns bigger structs through a hidden pointer */
	retp = cdataptr(rcd);
	stack[nstack++] = lj_guest_h2g(retp);
      }
      gcsteps++;
    }
    fid = ct->sib;
    for (o = L->base+1, narg = 1; o < top; o++, narg++) {
      CTypeID did;
      CType *d;
      CTSize dsz;
      uint8_t buf[64];
      int n, isfp;
      if (fid) {
	CType *ctf = ctype_get(cts, fid);
	fid = ctf->sib;
	did = ctype_cid(ctf->info);
      } else {
	if (!(info & CTF_VARARG))
	  lj_err_caller(L, LJ_ERR_FFI_NUMARG);
	did = lj_ccall_ctid_vararg(cts, o);
      }
      d = ctype_raw(cts, did);
      dsz = d->size;
      if (ctype_isnum(d->info)) {
	if (dsz > 8) lj_err_caller(L, LJ_ERR_FFI_NYICALL);
      } else if (ctype_isstruct(d->info)) {
	if (dsz > sizeof(buf)) lj_err_caller(L, LJ_ERR_FFI_NYICALL);
      } else if (ctype_iscomplex(d->info) || ctype_isvector(d->info)) {
	lj_err_caller(L, LJ_ERR_FFI_NYICALL);
      } else {
	dsz = CTSIZE_PTR;
      }
      memset(buf, 0, sizeof(buf));
      lj_cconv_ct_tv(cts, d, buf, o, CCF_ARG(narg));
      d = ctype_raw(cts, did);  /* cts->tab may have been reallocated */
      isfp = ctype_isfp(d->info);
      /* Sign-extend small signed integers (the callee may read 32 bits). */
      if (ctype_isinteger_or_bool(d->info) && dsz < 4 && !(d->info & CTF_UNSIGNED)) {
	int32_t v = dsz == 1 ? (int8_t)buf[0] : (int16_t)(buf[0] | buf[1] << 8);
	memcpy(buf, &v, 4);
      }
      n = (int)((dsz + 3) / 4);
      if (n < 1) n = 1;
      if (nreg < maxreg && n == 1 && !isfp && !ctype_isstruct(d->info)) {
	memcpy(&reg[nreg++], buf, 4);
	continue;
      }
      if (nstack + n > GUEST_MAXSTACK) lj_err_caller(L, LJ_ERR_FFI_NYICALL);
      memcpy(&stack[nstack], buf, (size_t)n * 4);
      nstack += n;
    }
    if (fid) lj_err_caller(L, LJ_ERR_FFI_NUMARG);  /* too few arguments */
    ctr = ctype_rawchild(cts, ctype_get(cts, id));
    lj_guest_ffi_call(target, stack, nstack, nreg, reg[0], reg[1], &eax, &edx, &st0,
		      ctype_isfp(ctr->info));
    ct = ctype_get(cts, id);  /* table may have been reallocated */
    ctr = ctype_rawchild(cts, ct);
    if (ctype_isvoid(ctr->info)) {
      ret = 0;
    } else if (ctype_isstruct(ctr->info) || ctype_iscomplex(ctr->info)) {
      ret = 1;
      if (!retp) {  /* small struct in EDX:EAX */
	uint8_t *dp = (uint8_t *)cdataptr(cdataV(L->top-1));
	uint32_t r2[2] = {eax, edx};
	memcpy(dp, r2, ctr->size);
      }
    } else {
      uint8_t buf[8];
      ret = 1;
      if (ctype_isfp(ctr->info)) {
	if (ctr->size == 4) { float f = (float)st0; memcpy(buf, &f, 4); }
	else memcpy(buf, &st0, 8);
      } else {
	uint32_t r2[2] = {eax, edx};
	memcpy(buf, r2, 8);
      }
      gcsteps += lj_cconv_tv_ct(cts, ctr, 0, L->top-1, buf);
    }
    while (gcsteps-- > 0)
      lj_gc_check(L);
    return ret;
  }
  return -1;  /* Not a function. */
}
