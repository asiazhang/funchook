/* -*- indent-tabs-mode: nil -*-
 *
 * This file is part of Duckhook.
 * https://github.com/kubo/duckhook
 *
 * Duckhook is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 2 of the License, or (at your
 * option) any later version.
 *
 * As a special exception, the copyright holders of this library give you
 * permission to link this library with independent modules to produce an
 * executable, regardless of the license terms of these independent
 * modules, and to copy and distribute the resulting executable under
 * terms of your choice, provided that you also meet, for each linked
 * independent module, the terms and conditions of the license of that
 * module. An independent module is a module which is not derived from or
 * based on this library. If you modify this library, you may extend this
 * exception to your version of the library, but you are not obliged to
 * do so. If you do not wish to do so, delete this exception statement
 * from your version.
 *
 * Duckhook is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Duckhook. If not, see <http://www.gnu.org/licenses/>.
 */
#define PSAPI_VERSION 1
#include <stdint.h>
#include <windows.h>
#include <psapi.h>
#include "duckhook_internal.h"

typedef struct page_info {
    struct page_info *next;
    struct page_info *prev;
    int num_used;
    char used[1];
} page_list_t;

static size_t allocation_unit; /* 64K */
static size_t page_size; /* 4K */
static size_t max_num_pages; /* 15 */
static page_list_t page_list = {
    &page_list,
    &page_list,
};

size_t duckhook_page_size(duckhook_t *duckhook)
{
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    page_size = si.dwPageSize;
    allocation_unit = si.dwAllocationGranularity;
    max_num_pages = allocation_unit / page_size - 1;
    return page_size;
}

/* Reserve 64K bytes (allocation_unit) and use the first
 * 4K bytes (1 page) as the control page.
 */
static int alloc_page_info(duckhook_t *duckhook, page_list_t **pl_out, void *hint)
{
    void *addr;
    page_list_t *pl;
#ifdef CPU_X86_64
    void *old_hint = hint;
    while (1) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(hint, &mbi, sizeof(mbi)) == 0) {
            duckhook_set_error_message(duckhook, "Virtual Query Error: addr=%p, error=%lu\n",
                                       hint, GetLastError());
            return DUCKHOOK_ERROR_INTERNAL_ERROR;
        }
        duckhook_log(duckhook, "  process map: %016I64x-%016I64x %s\n",
                     (size_t)mbi.BaseAddress, (size_t)mbi.BaseAddress + mbi.RegionSize,
                     (mbi.State == MEM_FREE) ? "free" : "used");
        if (mbi.State == MEM_FREE) {
            size_t addr = ROUND_UP((size_t)mbi.BaseAddress, allocation_unit);
            int diff = addr - (size_t)mbi.BaseAddress;
            if (diff >= 0) {
                if (mbi.RegionSize - diff >= allocation_unit) {
                    hint = (void*)addr;
                    duckhook_log(duckhook, "  change hint address from %p to %p\n",
                                 old_hint, hint);
                    break;
                }
            }
        }
        hint = (void*)((size_t)mbi.BaseAddress + mbi.RegionSize);
    }
#else
    hint = NULL;
#endif
    pl = VirtualAlloc(hint, allocation_unit, MEM_RESERVE, PAGE_NOACCESS);
    if (pl == NULL) {
        duckhook_set_error_message(duckhook, "Failed to reserve memory %p (hint=%p, size=%"SIZE_T_FMT"u, errro=%lu)\n",
                                   pl, hint, allocation_unit, GetLastError());
        return DUCKHOOK_ERROR_MEMORY_ALLOCATION;
    }
    duckhook_log(duckhook, "  reserve memory %p (hint=%p, size=%"SIZE_T_FMT"u)\n", pl, hint, allocation_unit);
    addr = VirtualAlloc(pl, page_size, MEM_COMMIT, PAGE_READWRITE);
    if (addr == NULL) {
        duckhook_set_error_message(duckhook, "Failed to commit memory %p for read-write (hint=%p, size=%"SIZE_T_FMT"u)\n",
                                   addr, pl, page_size);
        VirtualFree(pl, 0, MEM_RELEASE);
        return DUCKHOOK_ERROR_INTERNAL_ERROR;
    }
    duckhook_log(duckhook, "  commit memory %p for read-write (hint=%p, size=%"SIZE_T_FMT"u)\n", addr, pl, page_size);
    pl->next = page_list.next;
    pl->prev = &page_list;
    page_list.next->prev = pl;
    page_list.next = pl;
    *pl_out = pl;
    return 0;
}

/*
 * Get one page from page_list, commit it and return it.
 */
int duckhook_page_alloc(duckhook_t *duckhook, duckhook_page_t **page_out, uint8_t *func, rip_displacement_t *disp)
{
    page_list_t *pl;
    duckhook_page_t *page;
    int i;

    for (pl = page_list.next; pl != &page_list; pl = pl->next) {
        for (i = 0; i < max_num_pages; i++) {
            if (!pl->used[i]) {
                page = (duckhook_page_t *)((size_t)pl + (i + 1) * page_size);
                if (duckhook_page_avail(duckhook, page, 0, func, disp)) {
                    break;
                }
            }
        }
    }
    if (pl == &page_list) {
        /* no page_list is available. */
        int rv = alloc_page_info(duckhook, &pl, func);
        if (rv != 0) {
            return rv;
        }
        i = 0;
        page = (duckhook_page_t *)((size_t)pl + page_size);
    }
    if (VirtualAlloc(page, page_size, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        duckhook_set_error_message(duckhook, "Failed to commit page %p (base=%p(used=%d), idx=%d, size=%"SIZE_T_FMT"u, error=%lu)",
                                   page, pl, pl->num_used, i, page_size, GetLastError());
        return DUCKHOOK_ERROR_INTERNAL_ERROR;
    }
    pl->used[i] = 1;
    pl->num_used++;
    duckhook_log(duckhook, "  commit page %p (base=%p(used=%d), idx=%d, size=%"SIZE_T_FMT"u)\n",
                 page, pl, pl->num_used, i, page_size);
    *page_out = page;
    return 0;
}

/*
 * Back to one page to page_list.
 */
int duckhook_page_free(duckhook_t *duckhook, duckhook_page_t *page)
{
    page_list_t *pl = (page_list_t *)((size_t)page & ~(allocation_unit - 1));
    size_t idx = ((size_t)page - (size_t)pl) / page_size - 1;
    BOOL ok;

    ok = VirtualFree(page, page_size, MEM_DECOMMIT);
    duckhook_log(duckhook, "  %sdecommit page %p (base=%p(used=%d), idx=%"SIZE_T_FMT"u, size=%"SIZE_T_FMT"u)\n",
                 ok ? "" : "failed to ",
                 page, pl, pl->num_used, idx, page_size);
    if (!ok) {
        return -1;
    }
    pl->num_used--;
    pl->used[idx] = 0;
    if (pl->num_used != 0) {
        return 0;
    }
    /* all pages are decommitted. delete this page_list */
    pl->next->prev = pl->prev;
    pl->prev->next = pl->next;
    ok = VirtualFree(pl, 0, MEM_RELEASE);
    duckhook_log(duckhook, "  %srelease memory %p (size=%"SIZE_T_FMT"u)\n",
                 ok ? "" : "failed to ",
                 pl, allocation_unit);
    return ok ? 0 : -1;
}

int duckhook_page_protect(duckhook_t *duckhook, duckhook_page_t *page)
{
    BOOL ok = VirtualProtect(page, page_size, PAGE_EXECUTE_READ, NULL);
    duckhook_log(duckhook, "  %sprotect page %p (size=%"SIZE_T_FMT"u, prot=read,exec)\n",
                 ok ? "" : "failed to ",
                 page, page_size);
    return ok ? 0 : -1;
}

int duckhook_page_unprotect(duckhook_t *duckhook, duckhook_page_t *page)
{
    BOOL ok = VirtualProtect(page, page_size, PAGE_READWRITE, NULL);
    duckhook_log(duckhook, "  %sunprotect page %p (size=%"SIZE_T_FMT"u, prot=read,write)\n",
                 ok ? "" : "failed to ",
                 page, page_size);
    return ok ? 0 : -1;
}

int duckhook_unprotect_begin(duckhook_t *duckhook, mem_state_t *mstate, void *start, size_t len)
{
    size_t saddr = ROUND_DOWN((size_t)start, page_size);
    BOOL ok;

    mstate->addr = (void*)saddr;
    mstate->size = len + (size_t)start - saddr;
    mstate->size = ROUND_UP(mstate->size, page_size);
    ok = VirtualProtect(mstate->addr, mstate->size, PAGE_EXECUTE_READWRITE, &mstate->protect);
    duckhook_log(duckhook, "  %sunprotect memory %p (size=%"SIZE_T_FMT"u) <- %p (size=%"SIZE_T_FMT"u)\n",
                 ok ? "" : "failed to ",
                 mstate->addr, mstate->size, start, len);
    return ok ? 0 : -1;
}

int duckhook_unprotect_end(duckhook_t *duckhook, const mem_state_t *mstate)
{
    BOOL ok = VirtualProtect(mstate->addr, mstate->size, mstate->protect, NULL);
    duckhook_log(duckhook, "  %sprotect memory %p (size=%"SIZE_T_FMT"u)\n",
                 ok ? "" : "failed to ",
                 mstate->addr, mstate->size);
    return ok ? 0 : -1;
}

void *duckhook_resolve_func(duckhook_t *duckhook, void *func)
{
    if (duckhook_debug_file != NULL) {
        char path[PATH_MAX];
        DWORD len = GetMappedFileNameA(GetCurrentProcess(), func, path, sizeof(path));
        if (len > 0) {
            duckhook_log(duckhook, "  func %p is in %.*s\n", func, (int)len, path);
        }
    }
    return func;
}
