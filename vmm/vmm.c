// vmm.c : implementation of functions related to virtual memory management support.
//
// (c) Ulf Frisk, 2018-2020
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "vmm.h"
#include "mm.h"
#include "ob.h"
#include "pdb.h"
#include "vmmproc.h"
#include "vmmwin.h"
#include "vmmwindef.h"
#include "vmmwinobj.h"
#include "vmmwinreg.h"
#include "vmmwinnet.h"
#include "pluginmanager.h"
#include "util.h"
#include <sddl.h>

// ----------------------------------------------------------------------------
// CACHE FUNCTIONALITY:
// PHYSICAL MEMORY CACHING FOR READS AND PAGE TABLES
// ----------------------------------------------------------------------------

/*
* Retrieve cache table from ctxVmm given a specific tag.
*/
PVMM_CACHE_TABLE VmmCacheTableGet(_In_ DWORD wTblTag)
{
    switch(wTblTag) {
        case VMM_CACHE_TAG_PHYS:
            return &ctxVmm->Cache.PHYS;
        case VMM_CACHE_TAG_TLB:
            return &ctxVmm->Cache.TLB;
        case VMM_CACHE_TAG_PAGING:
            return &ctxVmm->Cache.PAGING;
        default:
            return NULL;
    }
}

#define VMM_CACHE2_GET_REGION(qwA)      ((qwA >> 12) % VMM_CACHE2_REGIONS)
#define VMM_CACHE2_GET_BUCKET(qwA)      ((qwA >> 12) % VMM_CACHE2_BUCKETS)

/*
* Invalidate a cache entry (if exists)
*/
VOID VmmCacheInvalidate_2(_In_ DWORD dwTblTag, _In_ QWORD qwA)
{
    DWORD iR, iB;
    PVMM_CACHE_TABLE t;
    PVMMOB_MEM pOb, pObNext;
    t = VmmCacheTableGet(dwTblTag);
    if(!t || !t->fActive) { return; }
    iR = VMM_CACHE2_GET_REGION(qwA);
    iB = VMM_CACHE2_GET_BUCKET(qwA);
    EnterCriticalSection(&t->R[iR].Lock);
    pOb = t->R[iR].B[iB];
    while(pOb) {
        pObNext = pOb->FLink;
        if(pOb->h.qwA == qwA) {
            // detach bucket
            if(pOb->BLink) {
                pOb->BLink->FLink = pOb->FLink;
            } else {
                t->R[iR].B[iB] = pOb->FLink;
            }
            if(pOb->FLink) {
                pOb->FLink->BLink = pOb->BLink;
            }
            // detach age list
            if(pOb->AgeBLink) {
                pOb->AgeBLink->AgeFLink = pOb->AgeFLink;
            } else {
                t->R[iR].AgeFLink = pOb->AgeFLink;
            }
            if(pOb->AgeFLink) {
                pOb->AgeFLink->AgeBLink = pOb->AgeBLink;
            } else {
                t->R[iR].AgeBLink = pOb->AgeBLink;
            }
            // decrease count & decref
            InterlockedDecrement(&t->R[iR].c);
            Ob_DECREF(pOb);
        }
        pOb = pObNext;
    }
    LeaveCriticalSection(&t->R[iR].Lock);
}

VOID VmmCacheInvalidate(_In_ QWORD pa)
{
    VmmCacheInvalidate_2(VMM_CACHE_TAG_TLB, pa);
    VmmCacheInvalidate_2(VMM_CACHE_TAG_PHYS, pa);
}

VOID VmmCacheReclaim(_In_ PVMM_CACHE_TABLE t, _In_ DWORD iR, _In_ BOOL fTotal)
{
    DWORD cThreshold;
    PVMMOB_MEM pOb;
    EnterCriticalSection(&t->R[iR].Lock);
    cThreshold = fTotal ? 0 : max(0x10, t->R[iR].c >> 1);
    while(t->R[iR].c > cThreshold) {
        // get
        pOb = t->R[iR].AgeBLink;
        if(!pOb) {
            vmmprintf_fn("ERROR - SHOULD NOT HAPPEN - NULL OBJECT RETRIEVED\n");
            break;
        }
        // detach from age list
        t->R[iR].AgeBLink = pOb->AgeBLink;
        if(pOb->AgeBLink) {
            pOb->AgeBLink->AgeFLink = NULL;
        } else {
            t->R[iR].AgeFLink = NULL;
        }
        // detach from bucket list
        if(pOb->BLink) {
            pOb->BLink->FLink = NULL;
        } else {
            t->R[iR].B[VMM_CACHE2_GET_BUCKET(pOb->h.qwA)] = NULL;
        }
        // remove region refcount of object - callback will take care of
        // re-insertion into empty list when refcount becomes low enough.
        Ob_DECREF(pOb);
        InterlockedDecrement(&t->R[iR].c);
    }
    LeaveCriticalSection(&t->R[iR].Lock);
}

/*
* Clear the specified cache from all entries.
* -- wTblTag
*/
VOID VmmCacheClear(_In_ DWORD dwTblTag)
{
    DWORD i;
    PVMM_CACHE_TABLE t;
    PVMM_PROCESS pObProcess = NULL;
    // 1: clear cache
    t = VmmCacheTableGet(dwTblTag);
    for(i = 0; i < VMM_CACHE2_REGIONS; i++) {
        VmmCacheReclaim(t, i, TRUE);
    }
    // 2: if tlb cache clear -> update process 'is spider done' flag
    if(dwTblTag == VMM_CACHE_TAG_TLB) {
        while((pObProcess = VmmProcessGetNext(pObProcess, 0))) {
            if(pObProcess->fTlbSpiderDone) {
                EnterCriticalSection(&pObProcess->LockUpdate);
                pObProcess->fTlbSpiderDone = FALSE;
                LeaveCriticalSection(&pObProcess->LockUpdate);
            }
        }
    }
}

VOID VmmCache_CallbackRefCount1(PVMMOB_MEM pOb)
{
    PVMM_CACHE_TABLE t;
    t = VmmCacheTableGet(((POB)pOb)->_tag);
    if(!t) {
        vmmprintf_fn("ERROR - SHOULD NOT HAPPEN - INVALID OBJECT TAG %02X\n", ((POB)pOb)->_tag);
        return;
    }
    if(!t->fActive) { return; }
    Ob_INCREF(pOb);
    InterlockedPushEntrySList(&t->ListHeadEmpty, &pOb->SListEmpty);
    InterlockedIncrement(&t->cEmpty);
}

/*
* Return an entry retrieved with VmmCacheReserve to the cache.
* NB! no other items may be returned with this function!
* FUNCTION DECREF: pOb
* -- pOb
*/
VOID VmmCacheReserveReturn(_In_opt_ PVMMOB_MEM pOb)
{
    DWORD iR, iB;
    PVMM_CACHE_TABLE t;
    if(!pOb) { return; }
    t = VmmCacheTableGet(((POB)pOb)->_tag);
    if(!t) {
        vmmprintf_fn("ERROR - SHOULD NOT HAPPEN - INVALID OBJECT TAG %02X\n", ((POB)pOb)->_tag);
        return;
    }
    if(!t->fActive || !pOb->h.f || (pOb->h.qwA == MEM_SCATTER_ADDR_INVALID)) {
        // decrement refcount of object - callback will take care of
        // re-insertion into empty list when refcount becomes low enough.
        Ob_DECREF(pOb);
        return;
    }
    // insert into map - refcount will be overtaken by "cache region".
    iR = VMM_CACHE2_GET_REGION(pOb->h.qwA);
    iB = VMM_CACHE2_GET_BUCKET(pOb->h.qwA);
    EnterCriticalSection(&t->R[iR].Lock);
    // insert into "bucket"
    pOb->BLink = NULL;
    pOb->FLink = t->R[iR].B[iB];
    if(pOb->FLink) { pOb->FLink->BLink = pOb; }
    t->R[iR].B[iB] = pOb;
    // insert into "age list"
    pOb->AgeFLink = t->R[iR].AgeFLink;
    if(pOb->AgeFLink) { pOb->AgeFLink->AgeBLink = pOb; }
    pOb->AgeBLink = NULL;
    t->R[iR].AgeFLink = pOb;
    if(!t->R[iR].AgeBLink) { t->R[iR].AgeBLink = pOb; }
    InterlockedIncrement(&t->R[iR].c);
    LeaveCriticalSection(&t->R[iR].Lock);
}

PVMMOB_MEM VmmCacheReserve(_In_ DWORD dwTblTag)
{
    PVMM_CACHE_TABLE t;
    PVMMOB_MEM pOb;
    PSLIST_ENTRY e;
    WORD iReclaimLast, cLoopProtect = 0;
    t = VmmCacheTableGet(dwTblTag);
    if(!t || !t->fActive) { return NULL; }
    while(!(e = InterlockedPopEntrySList(&t->ListHeadEmpty))) {
        if(t->cTotal < VMM_CACHE2_MAX_ENTRIES) {
            // below max threshold -> create new
            pOb = Ob_Alloc(t->tag, LMEM_ZEROINIT, sizeof(VMMOB_MEM), NULL, VmmCache_CallbackRefCount1);
            if(!pOb) { return NULL; }
            pOb->h.version = MEM_SCATTER_VERSION;
            pOb->h.cb = 0x1000;
            pOb->h.pb = pOb->pb;
            pOb->h.qwA = MEM_SCATTER_ADDR_INVALID;
            Ob_INCREF(pOb);  // "total list" reference
            InterlockedPushEntrySList(&t->ListHeadTotal, &pOb->SListTotal);
            InterlockedIncrement(&t->cTotal);
            return pOb;         // return fresh object - refcount = 2.
        }
        // reclaim existing entries
        iReclaimLast = InterlockedIncrement16(&t->iReclaimLast);
        VmmCacheReclaim(t, iReclaimLast % VMM_CACHE2_REGIONS, FALSE);
        if(++cLoopProtect == VMM_CACHE2_REGIONS) {
            vmmprintf_fn("ERROR - SHOULD NOT HAPPEN - CACHE %04X DRAINED OF ENTRIES\n", dwTblTag);
            Sleep(10);
        }
    }
    InterlockedDecrement(&t->cEmpty);
    pOb = CONTAINING_RECORD(e, VMMOB_MEM, SListEmpty);
    pOb->h.qwA = MEM_SCATTER_ADDR_INVALID;
    pOb->h.f = FALSE;
    return pOb; // reference overtaken by callee (from EmptyList)
}

PVMMOB_MEM VmmCacheGet(_In_ DWORD dwTblTag, _In_ QWORD qwA)
{
    PVMM_CACHE_TABLE t;
    DWORD iR;
    PVMMOB_MEM pOb;
    t = VmmCacheTableGet(dwTblTag);
    if(!t || !t->fActive) { return NULL; }
    iR = VMM_CACHE2_GET_REGION(qwA);
    EnterCriticalSection(&t->R[iR].Lock);
    pOb = t->R[iR].B[VMM_CACHE2_GET_BUCKET(qwA)];
    while(pOb && (qwA != pOb->h.qwA)) {
        pOb = pOb->FLink;
    }
    Ob_INCREF(pOb);
    LeaveCriticalSection(&t->R[iR].Lock);
    return pOb;
}

PVMMOB_MEM VmmCacheGet_FromDeviceOnMiss(_In_ DWORD dwTblTag, _In_ DWORD dwTblTagSecondaryOpt, _In_ QWORD qwA)
{
    PVMMOB_MEM pObMEM, pObReservedMEM;
    PMEM_SCATTER pMEM;
    pObMEM = VmmCacheGet(dwTblTag, qwA);
    if(pObMEM) { return pObMEM; }
    if((pObReservedMEM = VmmCacheReserve(dwTblTag))) {
        pMEM = &pObReservedMEM->h;
        pMEM->qwA = qwA;
        if(dwTblTagSecondaryOpt && (pObMEM = VmmCacheGet(dwTblTagSecondaryOpt, qwA))) {
            pMEM->f = TRUE;
            memcpy(pMEM->pb, pObMEM->pb, 0x1000);
            Ob_DECREF(pObMEM);
            pObMEM = NULL;
        }
        if(!pMEM->f) {
            LcReadScatter(ctxMain->hLC, 1, &pMEM);
        }
        if(pMEM->f) {
            Ob_INCREF(pObReservedMEM);
            VmmCacheReserveReturn(pObReservedMEM);
            return pObReservedMEM;
        }
        VmmCacheReserveReturn(pObReservedMEM);
    }
    return NULL;
}

BOOL VmmCacheExists(_In_ DWORD dwTblTag, _In_ QWORD qwA)
{
    BOOL result;
    PVMMOB_MEM pOb;
    pOb = VmmCacheGet(dwTblTag, qwA);
    result = pOb != NULL;
    Ob_DECREF(pOb);
    return result;
}

/*
* Retrieve a page table from a given physical address (if possible).
* CALLER DECREF: return
* -- pa
* -- fCacheOnly
* -- return = Cache entry on success, NULL on fail.
*/
PVMMOB_MEM VmmTlbGetPageTable(_In_ QWORD pa, _In_ BOOL fCacheOnly)
{
    PVMMOB_MEM pObMEM;
    pObMEM = VmmCacheGet(VMM_CACHE_TAG_TLB, pa);
    if(pObMEM) {
        InterlockedIncrement64(&ctxVmm->stat.cTlbCacheHit);
        return pObMEM;
    }
    if(fCacheOnly) { return NULL; }
    // try retrieve from (1) TLB cache, (2) PHYS cache, (3) device
    pObMEM = VmmCacheGet_FromDeviceOnMiss(VMM_CACHE_TAG_TLB, VMM_CACHE_TAG_PHYS, pa);
    if(!pObMEM) {
        InterlockedIncrement64(&ctxVmm->stat.cTlbReadFail);
        return NULL;
    }
    InterlockedIncrement64(&ctxVmm->stat.cTlbReadSuccess);
    if(VmmTlbPageTableVerify(pObMEM->h.pb, pObMEM->h.qwA, FALSE)) {
        return pObMEM;
    }
    Ob_DECREF(pObMEM);
    return NULL;
}

VOID VmmCache2Close(_In_ DWORD dwTblTag)
{
    PVMM_CACHE_TABLE t;
    PVMMOB_MEM pOb;
    PSLIST_ENTRY e;
    DWORD i;
    t = VmmCacheTableGet(dwTblTag);
    if(!t || !t->fActive) { return; }
    t->fActive = FALSE;
    // remove from "regions"
    for(i = 0; i < VMM_CACHE2_REGIONS; i++) {
        VmmCacheReclaim(t, i, TRUE);
        DeleteCriticalSection(&t->R[i].Lock);
    }
    // remove from "empty list"
    while(e = InterlockedPopEntrySList(&t->ListHeadEmpty)) {
        pOb = CONTAINING_RECORD(e, VMMOB_MEM, SListEmpty);
        Ob_DECREF(pOb);
        InterlockedDecrement(&t->cEmpty);
    }
    // remove from "total list"
    while(e = InterlockedPopEntrySList(&t->ListHeadTotal)) {
        pOb = CONTAINING_RECORD(e, VMMOB_MEM, SListTotal);
        Ob_DECREF(pOb);
        InterlockedDecrement(&t->cTotal);
    }
}

VOID VmmCache2Initialize(_In_ DWORD dwTblTag)
{
    DWORD i;
    PVMM_CACHE_TABLE t;
    t = VmmCacheTableGet(dwTblTag);
    if(!t || t->fActive) { return; }
    for(i = 0; i < VMM_CACHE2_REGIONS; i++) {
        InitializeCriticalSection(&t->R[i].Lock);
    }
    InitializeSListHead(&t->ListHeadEmpty);
    InitializeSListHead(&t->ListHeadTotal);
    t->fActive = TRUE;
    t->tag = dwTblTag;
}

/*
* Prefetch a set of physical addresses contained in pTlbPrefetch into the Tlb.
* NB! pTlbPrefetch must not be updated/altered during the function call.
* -- pProcess
* -- pTlbPrefetch = the page table addresses to prefetch (on entry) and empty set on exit.
*/
VOID VmmTlbPrefetch(_In_ POB_SET pTlbPrefetch)
{
    QWORD pbTlb = 0;
    DWORD cTlbs, i = 0;
    PPVMMOB_MEM ppObMEMs = NULL;
    PPMEM_SCATTER ppMEMs = NULL;
    if(!(cTlbs = ObSet_Size(pTlbPrefetch))) { goto fail; }
    if(!(ppMEMs = LocalAlloc(0, cTlbs * sizeof(PMEM_SCATTER)))) { goto fail; }
    if(!(ppObMEMs = LocalAlloc(0, cTlbs * sizeof(PVMMOB_MEM)))) { goto fail; }
    while((cTlbs = min(0x2000, ObSet_Size(pTlbPrefetch)))) {   // protect cache bleed -> max 0x2000 pages/round
        for(i = 0; i < cTlbs; i++) {
            ppObMEMs[i] = VmmCacheReserve(VMM_CACHE_TAG_TLB);
            ppMEMs[i] = &ppObMEMs[i]->h;
            ppMEMs[i]->qwA = ObSet_Pop(pTlbPrefetch);
        }
        LcReadScatter(ctxMain->hLC, cTlbs, ppMEMs);
        for(i = 0; i < cTlbs; i++) {
            if(ppMEMs[i]->f && !VmmTlbPageTableVerify(ppMEMs[i]->pb, ppMEMs[i]->qwA, FALSE)) {
                ppMEMs[i]->f = FALSE;  // "fail" invalid page table read
            }
            VmmCacheReserveReturn(ppObMEMs[i]);
        }
    }
fail:
    LocalFree(ppMEMs);
    LocalFree(ppObMEMs);
}

/*
* Prefetch a set of addresses contained in pPrefetchPages into the cache. This
* is useful when reading data from somewhat known addresses over higher latency
* connections.
* NB! pPrefetchPages must not be updated/altered during the function call.
* -- pProcess
* -- pPrefetchPages
* -- flags
*/
VOID VmmCachePrefetchPages(_In_opt_ PVMM_PROCESS pProcess, _In_opt_ POB_SET pPrefetchPages, _In_ QWORD flags)
{
    QWORD qwA = 0;
    DWORD cPages, iMEM = 0;
    PPMEM_SCATTER ppMEMs = NULL;
    cPages = ObSet_Size(pPrefetchPages);
    if(!cPages || (ctxVmm->flags & VMM_FLAG_NOCACHE)) { return; }
    if(!LcAllocScatter1(cPages, &ppMEMs)) { return; }
    while((qwA = ObSet_GetNext(pPrefetchPages, qwA))) {
        ppMEMs[iMEM++]->qwA = qwA & ~0xfff;
    }
    if(pProcess) {
        VmmReadScatterVirtual(pProcess, ppMEMs, iMEM, flags);
    } else {
        VmmReadScatterPhysical(ppMEMs, iMEM, flags);
    }
    LcMemFree(ppMEMs);
}

/*
* Prefetch a set of addresses. This is useful when reading data from somewhat
* known addresses over higher latency connections.
* -- pProcess
* -- cAddresses
* -- ... = variable list of total cAddresses of addresses of type QWORD.
*/
VOID VmmCachePrefetchPages2(_In_opt_ PVMM_PROCESS pProcess, _In_ DWORD cAddresses, ...)
{
    va_list arguments;
    POB_SET pObSet = NULL;
    if(!cAddresses || !(pObSet = ObSet_New())) { return; }
    va_start(arguments, cAddresses);
    while(cAddresses) {
        ObSet_Push(pObSet, va_arg(arguments, QWORD) & ~0xfff);
        cAddresses--;
    }
    va_end(arguments);
    VmmCachePrefetchPages(pProcess, pObSet, 0);
    Ob_DECREF(pObSet);
}

/*
* Prefetch a set of addresses contained in pPrefetchPagesNonPageAligned into
* the cache by first converting them to page aligned pages. This is used when
* reading data from somewhat known addresses over higher latency connections.
* NB! pPrefetchPagesNonPageAligned must not be altered during the function call.
* -- pProcess
* -- pPrefetchPagesNonPageAligned
* -- cb
* -- flags
*/
VOID VmmCachePrefetchPages3(_In_opt_ PVMM_PROCESS pProcess, _In_opt_ POB_SET pPrefetchPagesNonPageAligned, _In_ DWORD cb, _In_ QWORD flags)
{
    QWORD qwA = 0;
    POB_SET pObSetAlign;
    if(!cb || !pPrefetchPagesNonPageAligned) { return; }
    if(0 == ObSet_Size(pPrefetchPagesNonPageAligned)) { return; }
    if(!(pObSetAlign = ObSet_New())) { return; }
    while((qwA = ObSet_GetNext(pPrefetchPagesNonPageAligned, qwA))) {
        ObSet_Push_PageAlign(pObSetAlign, qwA, cb);
    }
    VmmCachePrefetchPages(pProcess, pObSetAlign, flags);
    Ob_DECREF(pObSetAlign);
}

/*
* Prefetch an array of optionally non-page aligned addresses. This is useful
* when reading data from somewhat known addresses over higher latency connections.
* -- pProcess
* -- cAddresses
* -- pqwAddresses = array of addresses to fetch
* -- cb
* -- flags
*/
VOID VmmCachePrefetchPages4(_In_opt_ PVMM_PROCESS pProcess, _In_ DWORD cAddresses, _In_ PQWORD pqwAddresses, _In_ DWORD cb, _In_ QWORD flags)
{
    POB_SET pObSet = NULL;
    if(!cAddresses || !(pObSet = ObSet_New())) { return; }
    while(cAddresses) {
        cAddresses--;
        if(pqwAddresses[cAddresses]) {
            ObSet_Push_PageAlign(pObSet, pqwAddresses[cAddresses], cb);
        }
    }
    VmmCachePrefetchPages(pProcess, pObSet, 0);
    Ob_DECREF(pObSet);
}

/*
* Prefetch memory of optionally non-page aligned addresses which are derived
* from pmPrefetchObjects by the pfnFilter filter function.
* -- pProcess
* -- pmPrefetch = map of objects.
* -- cb
* -- flags
* -- pfnFilter = filter as required by ObMap_FilterSet function.
* -- return = at least one object is found to be prefetched into cache.
*/
BOOL VmmCachePrefetchPages5(_In_opt_ PVMM_PROCESS pProcess, _In_opt_ POB_MAP pmPrefetch, _In_ DWORD cb, _In_ QWORD flags, _In_ VOID(*pfnFilter)(_In_ QWORD k, _In_ PVOID v, _Inout_ POB_SET ps))
{
    POB_SET psObCache = ObMap_FilterSet(pmPrefetch, pfnFilter);
    BOOL fResult = ObSet_Size(psObCache) > 0;
    VmmCachePrefetchPages3(pProcess, psObCache, cb, flags);
    Ob_DECREF(psObCache);
    return fResult;
}

// ----------------------------------------------------------------------------
// MAP FUNCTIONALITY BELOW: 
// SUPPORTED MAPS: PTE, VAD, MODULE, HEAP
// ----------------------------------------------------------------------------

/*
* Retrieve the PTE hardware page table memory map.
* CALLER DECREF: ppObPteMap
* -- pProcess
* -- ppObPteMap
* -- fExtendedText
* -- return
*/
_Success_(return)
BOOL VmmMap_GetPte(_In_ PVMM_PROCESS pProcess, _Out_ PVMMOB_MAP_PTE *ppObPteMap, _In_ BOOL fExtendedText)
{
    return
        (ctxVmm->tpMemoryModel != VMM_MEMORYMODEL_NA) &&
        ctxVmm->fnMemoryModel.pfnPteMapInitialize(pProcess) &&
        (!fExtendedText || VmmWinPte_InitializeMapText(pProcess)) &&
        (*ppObPteMap = Ob_INCREF(pProcess->Map.pObPte));
}

int VmmMap_GetPteEntry_CmpFind(_In_ QWORD vaFind, _In_ PVMM_MAP_PTEENTRY pEntry)
{
    if(pEntry->vaBase > vaFind) { return -1; }
    if(pEntry->vaBase + (pEntry->cPages << 12) - 1 < vaFind) { return 1; }
    return 0;
}

/*
* Retrieve a single PVMM_MAP_PTEENTRY from the PTE hardware page table memory map.
* -- pProcess
* -- ppObPteMap
* -- fExtendedText
* -- return = PTR to PTEENTRY or NULL on fail. Must not be used out of pPteMap scope.
*/
PVMM_MAP_PTEENTRY VmmMap_GetPteEntry(_In_ PVMMOB_MAP_PTE pPteMap, _In_ QWORD va)
{
    if(!pPteMap) { return NULL; }
    return Util_qfind((PVOID)va, pPteMap->cMap, pPteMap->pMap, sizeof(VMM_MAP_PTEENTRY), (int(*)(PVOID, PVOID))VmmMap_GetPteEntry_CmpFind);
}

/*
* Retrieve the VAD memory map.
* CALLER DECREF: ppObVadMap
* -- pProcess
* -- ppObVadMap
* -- fExtendedText
* -- return
*/
_Success_(return)
BOOL VmmMap_GetVad(_In_ PVMM_PROCESS pProcess, _Out_ PVMMOB_MAP_VAD *ppObVadMap, _In_ BOOL fExtendedText)
{
    if(!MmVad_MapInitialize(pProcess, fExtendedText, 0)) { return FALSE; }
    *ppObVadMap = Ob_INCREF(pProcess->Map.pObVad);
    return TRUE;
}

int VmmMap_GetVadEntry_CmpFind(_In_ QWORD vaFind, _In_ PVMM_MAP_VADENTRY pEntry)
{
    if(pEntry->vaStart > vaFind) { return -1; }
    if(pEntry->vaEnd < vaFind) { return 1; }
    return 0;
}

/*
* Retrieve a single PVMM_MAP_VADENTRY for a given VadMap and address inside it.
* -- pVadMap
* -- va
* -- return = PTR to VADENTRY or NULL on fail. Must not be used out of pVadMap scope.
*/
PVMM_MAP_VADENTRY VmmMap_GetVadEntry(_In_opt_ PVMMOB_MAP_VAD pVadMap, _In_ QWORD va)
{
    if(!pVadMap) { return NULL; }
    return Util_qfind((PVOID)va, pVadMap->cMap, pVadMap->pMap, sizeof(VMM_MAP_VADENTRY), (int(*)(PVOID, PVOID))VmmMap_GetVadEntry_CmpFind);
}

/*
* Retrieve the process module map.
* CALLER DECREF: ppObModuleMap
* -- pProcess
* -- ppObModuleMap
* -- return
*/
_Success_(return)
BOOL VmmMap_GetModule(_In_ PVMM_PROCESS pProcess, _Out_ PVMMOB_MAP_MODULE *ppObModuleMap)
{
    if(!pProcess->Map.pObModule && !VmmWinLdrModule_Initialize(pProcess)) { return FALSE; }
    *ppObModuleMap = Ob_INCREF(pProcess->Map.pObModule);
    return TRUE;
}

int VmmMap_GetModuleEntry_CmpFind(_In_ DWORD qwHash, _In_ PDWORD pdwEntry)
{
    if(*pdwEntry > qwHash) { return -1; }
    if(*pdwEntry < qwHash) { return 1; }
    return 0;
}

/*
* Retrieve a single PVMM_MAP_MODULEENTRY for a given ModuleMap and module name inside it.
* -- pModuleMap
* -- wszModuleName
* -- return = PTR to VMM_MAP_MODULEENTRY or NULL on fail. Must not be used out of pModuleMap scope.
*/
PVMM_MAP_MODULEENTRY VmmMap_GetModuleEntry(_In_ PVMMOB_MAP_MODULE pModuleMap, _In_ LPWSTR wszModuleName)
{
    QWORD qwHash, *pqwHashIndex;
    WCHAR wsz[MAX_PATH];
    Util_PathFileNameFixW(wsz, wszModuleName, 0);
    qwHash = Util_HashStringUpperW(wsz);
    pqwHashIndex = (PQWORD)Util_qfind((PVOID)qwHash, pModuleMap->cMap, pModuleMap->pHashTableLookup, sizeof(QWORD), (int(*)(PVOID, PVOID))VmmMap_GetModuleEntry_CmpFind);
    return pqwHashIndex ? &pModuleMap->pMap[*pqwHashIndex >> 32] : NULL;
}

/*
* Retrieve the heap map.
* CALLER DECREF: ppObHeapMap
* -- pProcess
* -- ppObHeapMap
* -- return
*/
_Success_(return)
BOOL VmmMap_GetHeap(_In_ PVMM_PROCESS pProcess, _Out_ PVMMOB_MAP_HEAP *ppObHeapMap)
{
    if(!pProcess->Map.pObHeap && !VmmWinHeap_Initialize(pProcess)) { return FALSE; }
    *ppObHeapMap = Ob_INCREF(pProcess->Map.pObHeap);
    return TRUE;
}

/*
* LPTHREAD_START_ROUTINE for VmmMap_GetThreadAsync.
*/
DWORD VmmMap_GetThreadAsync_ThreadStartRoutine(_In_ PVMM_PROCESS pProcess)
{
    VmmWinThread_Initialize(pProcess, TRUE);
    return 1;
}

/*
* Start async initialization of the thread map. This may be done to speed up
* retrieval of the thread map in the future since processing to retrieve it
* has already been progressing for a while. This may be useful for processes
* with large amount of threads - such as the system process.
* -- pProcess
*/
VOID VmmMap_GetThreadAsync(_In_ PVMM_PROCESS pProcess)
{
    VmmWork(VmmMap_GetThreadAsync_ThreadStartRoutine, pProcess, 0);
}

/*
* Retrieve the thread map.
* CALLER DECREF: ppObThreadMap
* -- pProcess
* -- ppObThreadMap
* -- return
*/
_Success_(return)
BOOL VmmMap_GetThread(_In_ PVMM_PROCESS pProcess, _Out_ PVMMOB_MAP_THREAD *ppObThreadMap)
{
    if(!pProcess->Map.pObThread && !VmmWinThread_Initialize(pProcess, FALSE)) { return FALSE; }
    *ppObThreadMap = Ob_INCREF(pProcess->Map.pObThread);
    return TRUE;
}

int VmmMap_GetThreadEntry_CmpFind(_In_ DWORD dwTID, _In_ PVMM_MAP_THREADENTRY pEntry)
{
    if(pEntry->dwTID > dwTID) { return -1; }
    if(pEntry->dwTID < dwTID) { return 1; }
    return 0;
}

/*
* Retrieve a single PVMM_MAP_THREADENTRY for a given ThreadMap and ThreadID.
* -- pThreadMap
* -- dwTID
* -- return = PTR to VMM_MAP_THREADENTRY or NULL on fail. Must not be used out of pThreadMap scope.
*/
PVMM_MAP_THREADENTRY VmmMap_GetThreadEntry(_In_ PVMMOB_MAP_THREAD pThreadMap, _In_ DWORD dwTID)
{
    QWORD qwTID = dwTID;
    return Util_qfind((PVOID)qwTID, pThreadMap->cMap, pThreadMap->pMap, sizeof(VMM_MAP_THREADENTRY), (int(*)(PVOID, PVOID))VmmMap_GetThreadEntry_CmpFind);
}

/*
* Retrieve the HANDLE map
* CALLER DECREF: ppObHandleMap
* -- pProcess
* -- ppObHandleMap
* -- fExtendedText
* -- return
*/
_Success_(return)
BOOL VmmMap_GetHandle(_In_ PVMM_PROCESS pProcess, _Out_ PVMMOB_MAP_HANDLE *ppObHandleMap, _In_ BOOL fExtendedText)
{
    if(!VmmWinHandle_Initialize(pProcess, fExtendedText)) { return FALSE; }
    *ppObHandleMap = Ob_INCREF(pProcess->Map.pObHandle);
    return TRUE;
}

/*
* Retrieve the Physical Memory Map.
* CALLER DECREF: ppObPhysMem
* -- ppObPhysMem
* -- return
*/
_Success_(return)
BOOL VmmMap_GetPhysMem(_Out_ PVMMOB_MAP_PHYSMEM *ppObPhysMem)
{
    PVMMOB_MAP_PHYSMEM pObPhysMemMap = ObContainer_GetOb(ctxVmm->pObCMapPhysMem);
    if(!pObPhysMemMap) {
        pObPhysMemMap = VmmWinPhysMemMap_Initialize();
    }
    *ppObPhysMem = pObPhysMemMap;
    return pObPhysMemMap != NULL;
}

/*
* Retrieve the USER map
* CALLER DECREF: ppObUserMap
* -- ppObUserMap
* -- return
*/
_Success_(return)
BOOL VmmMap_GetUser(_Out_ PVMMOB_MAP_USER *ppObUserMap)
{
    PVMMOB_MAP_USER pObUserMap = ObContainer_GetOb(ctxVmm->pObCMapUser);
    if(!pObUserMap) {
        pObUserMap = VmmWinUser_Initialize();
    }
    *ppObUserMap = pObUserMap;
    return pObUserMap != NULL;
}

/*
* Retrieve the NETWORK CONNECTION map
* CALLER DECREF: ppObNetMap
* -- ppObNetMap
* -- return
*/
_Success_(return)
BOOL VmmMap_GetNet(_Out_ PVMMOB_MAP_NET *ppObNetMap)
{
    PVMMOB_MAP_NET pObNetMap = ObContainer_GetOb(ctxVmm->pObCMapNet);
    if(!pObNetMap) {
        pObNetMap = VmmWinNet_Initialize();
    }
    *ppObNetMap = pObNetMap;
    return pObNetMap != NULL;
}

// ----------------------------------------------------------------------------
// PROCESS MANAGEMENT FUNCTIONALITY:
//
// The process 'object' represents a process in the analyzed system.
//
// The process 'object' is an object manager refcount object. The processes may
// contain, in addition to values, sub-objects such as maps of loaded modules
// and memory.
//
// Before updates to the process object happens the 'LockUpdate' generally
// should be acquired.
//
// The active processes are contained in a 'process table' which is also an
// object manager refcount object. Atmoic access (get and increase refcount) is
// guarded by a object manager container which allows for easy retrieval of the
// process table. The process table may also contain a process table for new
// not yet committed process objects. When processes are refreshed in the back-
// ground they are created (or copied by refcount increase) into the new table.
// Once all processes are enumerated the function 'VmmProcessCreateFinish' is
// called and replaces the 'old' table with the 'new' table which becomes the
// active table. The 'old' replaced table is refcount-decreased and possibly
// free'd as a result.
//
// The process object: VMM_PROCESS
// The process table object (only used internally): VMMOB_PROCESS_TABLE
// ----------------------------------------------------------------------------

VOID VmmProcess_TokenTryEnsure(_In_ PVMMOB_PROCESS_TABLE pt)
{
    BOOL f, f32 = ctxVmm->f32;
    DWORD j, i = 0, iM, cbHdr, cb;
    QWORD va, *pva = NULL;
    BYTE pb[0x1000];
    PVMM_PROCESS *ppProcess = NULL, pObSystemProcess = NULL;
    PVMM_OFFSET_EPROCESS oep = &ctxVmm->offset.EPROCESS;
    f = oep->opt.TOKEN_TokenId &&                                               // token offsets/symbols initialized.
        (pObSystemProcess = VmmProcessGet(4)) &&
        (pva = LocalAlloc(LMEM_ZEROINIT, pt->c * sizeof(QWORD))) &&
        (ppProcess = LocalAlloc(LMEM_ZEROINIT, pt->c * sizeof(PVMM_PROCESS)));
    if(!f) { goto fail; }
    cbHdr = f32 ? 0x2c : 0x5c;
    cb = cbHdr + oep->opt.TOKEN_UserAndGroups + 8;
    // 1: Get Process and Token VA:
    iM = pt->_iFLink;
    while(iM && i < pt->c) {
        if((ppProcess[i] = pt->_M[iM]) && !ppProcess[i]->win.TOKEN.fInitialized) {
            va = VMM_PTR_OFFSET(f32, ppProcess[i]->win.EPROCESS.pb, oep->opt.Token) & (f32 ? ~0x7 : ~0xf);
            if(VMM_KADDR(va)) {
                ppProcess[i]->win.TOKEN.va = va;
                pva[i] = va - cbHdr; // adjust for _OBJECT_HEADER and Pool Header
            }
        }
        iM = pt->_iFLinkM[iM];
        i++;
    }
    // 2: Read Token:
    VmmCachePrefetchPages4(pObSystemProcess, (DWORD)pt->c, pva, cb, 0);
    for(i = 0; i < pt->c; i++) {
        f = pva[i] && VmmRead2(pObSystemProcess, pva[i], pb, cb, VMM_FLAG_FORCECACHE_READ) &&
            (pva[i] = VMM_PTR_OFFSET(f32, pb, cb - 8)) &&
            VMM_KADDR(pva[i]);
        if(f) {
            for(j = 0, f = FALSE; !f && (j < cbHdr); j += (f32 ? 0x08 : 0x10)) {
                f = VMM_POOLTAG_SHORT(*(PDWORD)(pb + j), 'Toke');
            }
            if(f) {
                ppProcess[i]->win.TOKEN.qwLUID = *(PQWORD)(pb + cbHdr + ctxVmm->offset.EPROCESS.opt.TOKEN_TokenId);
                ppProcess[i]->win.TOKEN.dwSessionId = *(PDWORD)(pb + cbHdr + ctxVmm->offset.EPROCESS.opt.TOKEN_SessionId);
            }
        }
        if(!f) { pva[i] = 0; }
    }
    // 3: Read SID ptr:
    VmmCachePrefetchPages4(pObSystemProcess, (DWORD)pt->c, pva, 8, 0);
    for(i = 0; i < pt->c; i++) {
        f = pva[i] && VmmRead2(pObSystemProcess, pva[i], pb, 8, VMM_FLAG_FORCECACHE_READ) &&
            (pva[i] = VMM_PTR_OFFSET(f32, pb, 0)) &&
            VMM_KADDR(pva[i]);
        if(!f) { pva[i] = 0; };
    }
    // 4: Get SID:
    VmmCachePrefetchPages4(pObSystemProcess, (DWORD)pt->c, pva, SECURITY_MAX_SID_SIZE, 0);
    for(i = 0; i < pt->c; i++) {
        if(!ppProcess[i]) { continue; }
        ppProcess[i]->win.TOKEN.fSID =
            (va = pva[i]) &&
            VmmRead2(pObSystemProcess, va, (PBYTE)&ppProcess[i]->win.TOKEN.pbSID, SECURITY_MAX_SID_SIZE, VMM_FLAG_FORCECACHE_READ) &&
            IsValidSid(&ppProcess[i]->win.TOKEN.SID);
    }
    // 5: finish up:
    for(i = 0; i < pt->c; i++) {
        if(!ppProcess[i]) { continue; }
        ppProcess[i]->win.TOKEN.fSID =
            ppProcess[i]->win.TOKEN.fSID &&
            ConvertSidToStringSidA(&ppProcess[i]->win.TOKEN.SID, &ppProcess[i]->win.TOKEN.szSID) &&
            (ppProcess[i]->win.TOKEN.dwHashSID = Util_HashStringA(ppProcess[i]->win.TOKEN.szSID));
        ppProcess[i]->win.TOKEN.fInitialized = TRUE;
    }
fail:
    LocalFree(pva);
    LocalFree(ppProcess);
    Ob_DECREF(pObSystemProcess);
}

/*
* Global Synchronization/Lock of VmmProcess_TokenTryEnsure()
* -- pt
* -- pProcess
*/
VOID VmmProcess_TokenTryEnsureLock(_In_ PVMMOB_PROCESS_TABLE pt, _In_ PVMM_PROCESS pProcess)
{
    if(pProcess->win.TOKEN.fInitialized) { return; }
    EnterCriticalSection(&ctxVmm->LockMaster);
    if(!pProcess->win.TOKEN.fInitialized) {
        VmmProcess_TokenTryEnsure(pt);
    }
    LeaveCriticalSection(&ctxVmm->LockMaster);
}

/*
* Retrieve a process for a given PID and optional PVMMOB_PROCESS_TABLE.
* CALLER DECREF: return
* -- pt
* -- dwPID
* -- flags = 0 (recommended) or VMM_FLAG_PROCESS_TOKEN.
* -- return
*/
PVMM_PROCESS VmmProcessGetEx(_In_opt_ PVMMOB_PROCESS_TABLE pt, _In_ DWORD dwPID, _In_ QWORD flags)
{
    BOOL fToken = ((flags | ctxVmm->flags) & VMM_FLAG_PROCESS_TOKEN);
    PVMM_PROCESS pObProcess, pObProcessClone;
    PVMMOB_PROCESS_TABLE pObTable;
    DWORD i, iStart;
    if(!pt) {
        pObTable = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ctxVmm->pObCPROC);
        pObProcess = VmmProcessGetEx(pObTable, dwPID, flags);
        Ob_DECREF(pObTable);
        return pObProcess;
    }
    i = iStart = dwPID % VMM_PROCESSTABLE_ENTRIES_MAX;
    while(TRUE) {
        if(!pt->_M[i]) { goto fail; }
        if(pt->_M[i]->dwPID == dwPID) {
            pObProcess = (PVMM_PROCESS)Ob_INCREF(pt->_M[i]);
            if(pObProcess && fToken && !pObProcess->win.TOKEN.fInitialized) { VmmProcess_TokenTryEnsureLock(pt, pObProcess); }
            return pObProcess;
        }
        if(++i == VMM_PROCESSTABLE_ENTRIES_MAX) { i = 0; }
        if(i == iStart) { goto fail; }
    }
fail:
    if(dwPID & VMM_PID_PROCESS_CLONE_WITH_KERNELMEMORY) {
        if((pObProcess = VmmProcessGetEx(pt, dwPID & ~VMM_PID_PROCESS_CLONE_WITH_KERNELMEMORY, flags))) {
            if((pObProcessClone = VmmProcessClone(pObProcess))) {
                pObProcessClone->fUserOnly = FALSE;
            }
            Ob_DECREF(pObProcess);
            return pObProcessClone;
        }
    }
    return NULL;
}

/*
* Retrieve the next process given a process and a process table. This may be
* useful when iterating over a process list. NB! Listing of next item may fail
* prematurely if the previous process is terminated while having a reference
* to it.
* FUNCTION DECREF: pProcess
* CALLER DECREF: return
* -- pt
* -- pProcess = a process struct, or NULL if first.
*    NB! function DECREF's  pProcess and must not be used after call!
* -- flags = 0 (recommended) or VMM_FLAG_PROCESS_[TOKEN|SHOW_TERMINATED].
* -- return = a process struct, or NULL if not found.
*/
PVMM_PROCESS VmmProcessGetNextEx(_In_opt_ PVMMOB_PROCESS_TABLE pt, _In_opt_ PVMM_PROCESS pProcess, _In_ QWORD flags)
{
    BOOL fToken = ((flags | ctxVmm->flags) & VMM_FLAG_PROCESS_TOKEN);
    BOOL fShowTerminated = ((flags | ctxVmm->flags) & VMM_FLAG_PROCESS_SHOW_TERMINATED);
    PVMM_PROCESS pProcessNew;
    DWORD i, iStart;
    if(!pt) {
        pt = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ctxVmm->pObCPROC);
        if(!pt) { goto fail; }
        pProcessNew = VmmProcessGetNextEx(pt, pProcess, flags);
        Ob_DECREF(pt);
        return pProcessNew;
    }
restart:
    if(!pProcess) {
        i = pt->_iFLink;
        if(!pt->_M[i]) { goto fail; }
        pProcessNew = (PVMM_PROCESS)Ob_INCREF(pt->_M[i]);
        Ob_DECREF(pProcess);
        pProcess = pProcessNew;
        if(pProcess && pProcess->dwState && !fShowTerminated) { goto restart; }
        if(pProcess && fToken && !pProcess->win.TOKEN.fInitialized) { VmmProcess_TokenTryEnsureLock(pt, pProcess); }
        return pProcess;
    }
    i = iStart = pProcess->dwPID % VMM_PROCESSTABLE_ENTRIES_MAX;
    while(TRUE) {
        if(!pt->_M[i]) { goto fail; }
        if(pt->_M[i]->dwPID == pProcess->dwPID) {
            // current process -> retrieve next!
            i = pt->_iFLinkM[i];
            if(!pt->_M[i]) { goto fail; }
            pProcessNew = (PVMM_PROCESS)Ob_INCREF(pt->_M[i]);
            Ob_DECREF(pProcess);
            pProcess = pProcessNew;
            if(pProcess && pProcess->dwState && !fShowTerminated) { goto restart; }
            if(pProcess && fToken && !pProcess->win.TOKEN.fInitialized) { VmmProcess_TokenTryEnsureLock(pt, pProcess); }
            return pProcess;
        }
        if(++i == VMM_PROCESSTABLE_ENTRIES_MAX) { i = 0; }
        if(i == iStart) { goto fail; }
    }
fail:
    Ob_DECREF(pProcess);
    return NULL;
}

/*
* Object manager callback before 'static process' object cleanup
* decrease refcount of any internal objects.
*/
VOID VmmProcessStatic_CloseObCallback(_In_ PVOID pVmmOb)
{
    PVMMOB_PROCESS_PERSISTENT pProcessStatic = (PVMMOB_PROCESS_PERSISTENT)pVmmOb;
    Ob_DECREF_NULL(&pProcessStatic->pObCMapVadPrefetch);
    Ob_DECREF_NULL(&pProcessStatic->pObCLdrModulesPrefetch32);
    Ob_DECREF_NULL(&pProcessStatic->pObCLdrModulesPrefetch64);
    Ob_DECREF_NULL(&pProcessStatic->pObCMapThreadPrefetch);
    Ob_DECREF_NULL(&pProcessStatic->Plugin.pObCMiniDump);
    LocalFree(pProcessStatic->uszPathKernel);
    LocalFree(pProcessStatic->wszPathKernel);
    LocalFree(pProcessStatic->UserProcessParams.uszCommandLine);
    LocalFree(pProcessStatic->UserProcessParams.wszCommandLine);
    LocalFree(pProcessStatic->UserProcessParams.uszImagePathName);
    LocalFree(pProcessStatic->UserProcessParams.wszImagePathName);
}

/*
* Object manager callback before 'static process' object cleanup
* decrease refcount of any internal objects.
*/
VOID VmmProcessStatic_Initialize(_In_ PVMM_PROCESS pProcess)
{
    EnterCriticalSection(&pProcess->LockUpdate);
    Ob_DECREF_NULL(&pProcess->pObPersistent);
    pProcess->pObPersistent = Ob_Alloc(OB_TAG_VMM_PROCESS_PERSISTENT, LMEM_ZEROINIT, sizeof(VMMOB_PROCESS_PERSISTENT), VmmProcessStatic_CloseObCallback, NULL);
    if(pProcess->pObPersistent) {
        pProcess->pObPersistent->pObCMapVadPrefetch = ObContainer_New(NULL);
        pProcess->pObPersistent->pObCLdrModulesPrefetch32 = ObContainer_New(NULL);
        pProcess->pObPersistent->pObCLdrModulesPrefetch64 = ObContainer_New(NULL);
        pProcess->pObPersistent->pObCMapThreadPrefetch = ObContainer_New(NULL);
        pProcess->pObPersistent->Plugin.pObCMiniDump = ObContainer_New(NULL);
    }
    LeaveCriticalSection(&pProcess->LockUpdate);
}

/*
* Object manager callback before 'process' object cleanup - decrease refcount
* of any internal 'memory map' and 'module map' objects.
*/
VOID VmmProcess_CloseObCallback(_In_ PVOID pVmmOb)
{
    PVMM_PROCESS pProcess = (PVMM_PROCESS)pVmmOb;
    // general cleanup below
    Ob_DECREF(pProcess->Map.pObPte);
    Ob_DECREF(pProcess->Map.pObVad);
    Ob_DECREF(pProcess->Map.pObModule);
    Ob_DECREF(pProcess->Map.pObHeap);
    Ob_DECREF(pProcess->Map.pObThread);
    Ob_DECREF(pProcess->Map.pObHandle);
    Ob_DECREF(pProcess->pObPersistent);
    LocalFree(pProcess->win.TOKEN.szSID);
    // plugin cleanup below
    Ob_DECREF(pProcess->Plugin.pObCLdrModulesDisplayCache);
    Ob_DECREF(pProcess->Plugin.pObCPeDumpDirCache);
    Ob_DECREF(pProcess->Plugin.pObCPhys2Virt);
    // delete lock
    DeleteCriticalSection(&pProcess->LockUpdate);
    DeleteCriticalSection(&pProcess->LockPlugin);
    DeleteCriticalSection(&pProcess->Map.LockUpdateThreadMap);
    DeleteCriticalSection(&pProcess->Map.LockUpdateExtendedInfo);
}

VOID VmmProcessClone_CloseObCallback(_In_ PVOID pVmmOb)
{
    PVMM_PROCESS pProcessClone = (PVMM_PROCESS)pVmmOb;
    // decref clone parent
    Ob_DECREF(pProcessClone->pObProcessCloneParent);
    // delete lock
    DeleteCriticalSection(&pProcessClone->LockUpdate);
    DeleteCriticalSection(&pProcessClone->LockPlugin);
    DeleteCriticalSection(&pProcessClone->Map.LockUpdateThreadMap);
    DeleteCriticalSection(&pProcessClone->Map.LockUpdateExtendedInfo);
}

/*
* Object manager callback before 'process table' object cleanup - decrease
* refcount of all contained 'process' objects.
*/
VOID VmmProcessTable_CloseObCallback(_In_ PVOID pVmmOb)
{
    PVMMOB_PROCESS_TABLE pt = (PVMMOB_PROCESS_TABLE)pVmmOb;
    PVMM_PROCESS pProcess;
    WORD iProcess;
    // Close NewPROC
    Ob_DECREF_NULL(&pt->pObCNewPROC);
    // DECREF all pProcess in table
    iProcess = pt->_iFLink;
    pProcess = pt->_M[iProcess];
    while(pProcess) {
        Ob_DECREF(pProcess);
        iProcess = pt->_iFLinkM[iProcess];
        pProcess = pt->_M[iProcess];
        if(!pProcess || iProcess == pt->_iFLink) { break; }
    }
}

/*
* Clone an original process entry creating a shallow clone. The user of this
* shallow clone may use it to set the fUserOnly flag to FALSE on an otherwise
* user-mode process to be able to access the whole kernel space for a standard
* user-mode process.
* NB! USE WITH EXTREME CARE - MAY CRASH VMM IF USED MORE GENERALLY!
* CALLER DECREF: return
* -- pProcess
* -- return
*/
PVMM_PROCESS VmmProcessClone(_In_ PVMM_PROCESS pProcess)
{
    PVMM_PROCESS pObProcessClone;
    if(pProcess->pObProcessCloneParent) { return NULL; }
    pObProcessClone = (PVMM_PROCESS)Ob_Alloc(OB_TAG_VMM_PROCESS_CLONE, LMEM_ZEROINIT, sizeof(VMM_PROCESS), VmmProcessClone_CloseObCallback, NULL);
    if(!pObProcessClone) { return NULL; }
    memcpy((PBYTE)pObProcessClone + sizeof(OB), (PBYTE)pProcess + sizeof(OB), pProcess->ObHdr.cbData);
    pObProcessClone->pObProcessCloneParent = Ob_INCREF(pProcess);
    InitializeCriticalSection(&pObProcessClone->LockUpdate);
    InitializeCriticalSection(&pObProcessClone->LockPlugin);
    InitializeCriticalSection(&pObProcessClone->Map.LockUpdateThreadMap);
    InitializeCriticalSection(&pObProcessClone->Map.LockUpdateExtendedInfo);
    return pObProcessClone;
}

/*
* Create a new process object. New process object are created in a separate
* data structure and won't become visible to the "Process" functions until
* after the VmmProcessCreateFinish have been called.
* CALLER DECREF: return
* -- fTotalRefresh = create a completely new entry - i.e. do not copy any form
*                    of data from the old entry such as module and memory maps.
* -- dwPID
* -- dwPPID = parent PID (if any)
* -- dwState
* -- paDTB
* -- paDTB_UserOpt
* -- szName
* -- fUserOnly = user mode process (hide supervisor pages from view)
* -- pbEPROCESS
* -- cbEPROCESS
* -- return
*/
PVMM_PROCESS VmmProcessCreateEntry(_In_ BOOL fTotalRefresh, _In_ DWORD dwPID, _In_ DWORD dwPPID, _In_ DWORD dwState, _In_ QWORD paDTB, _In_ QWORD paDTB_UserOpt, _In_ CHAR szName[16], _In_ BOOL fUserOnly, _In_reads_opt_(cbEPROCESS) PBYTE pbEPROCESS, _In_ DWORD cbEPROCESS)
{
    PVMMOB_PROCESS_TABLE ptOld = NULL, ptNew = NULL;
    QWORD i, iStart, cEmpty = 0, cValid = 0;
    PVMM_PROCESS pProcess = NULL, pProcessOld = NULL;
    PVMMOB_MEM pObDTB = NULL;
    BOOL result;
    // 1: Sanity check DTB
    if(dwState == 0) {
        pObDTB = VmmTlbGetPageTable(paDTB & ~0xfff, FALSE);
        if(!pObDTB) { goto fail; }
        result = VmmTlbPageTableVerify(pObDTB->h.pb, paDTB, (ctxVmm->tpSystem == VMM_SYSTEM_WINDOWS_X64));
        Ob_DECREF(pObDTB);
        if(!result) { goto fail; }
    }
    // 2: Allocate new 'Process Table' (if not already existing)
    ptOld = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ctxVmm->pObCPROC);
    if(!ptOld) { goto fail; }
    ptNew = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ptOld->pObCNewPROC);
    if(!ptNew) {
        ptNew = (PVMMOB_PROCESS_TABLE)Ob_Alloc(OB_TAG_VMM_PROCESSTABLE, LMEM_ZEROINIT, sizeof(VMMOB_PROCESS_TABLE), VmmProcessTable_CloseObCallback, NULL);
        if(!ptNew) { goto fail; }
        ptNew->pObCNewPROC = ObContainer_New(NULL);
        ObContainer_SetOb(ptOld->pObCNewPROC, ptNew);
    }
    // 3: Sanity check - process to create not already in 'new' table.
    pProcess = VmmProcessGetEx(ptNew, dwPID, 0);
    if(pProcess) { goto fail; }
    // 4: Prepare existing item, or create new item, for new PID
    if(!fTotalRefresh) {
        pProcess = VmmProcessGetEx(ptOld, dwPID, 0);
    }
    if(!pProcess) {
        pProcess = (PVMM_PROCESS)Ob_Alloc(OB_TAG_VMM_PROCESS, LMEM_ZEROINIT, sizeof(VMM_PROCESS), VmmProcess_CloseObCallback, NULL);
        if(!pProcess) { goto fail; }
        InitializeCriticalSectionAndSpinCount(&pProcess->LockUpdate, 4096);
        InitializeCriticalSection(&pProcess->LockPlugin);
        InitializeCriticalSection(&pProcess->Map.LockUpdateThreadMap);
        InitializeCriticalSection(&pProcess->Map.LockUpdateExtendedInfo);
        memcpy(pProcess->szName, szName, 16);
        pProcess->szName[15] = 0;
        pProcess->dwPID = dwPID;
        pProcess->dwPPID = dwPPID;
        pProcess->dwState = dwState;
        pProcess->paDTB = paDTB;
        pProcess->paDTB_UserOpt = paDTB_UserOpt;
        pProcess->fUserOnly = fUserOnly;
        pProcess->fTlbSpiderDone = pProcess->fTlbSpiderDone;
        pProcess->Plugin.pObCLdrModulesDisplayCache = ObContainer_New(NULL);
        pProcess->Plugin.pObCPeDumpDirCache = ObContainer_New(NULL);
        pProcess->Plugin.pObCPhys2Virt = ObContainer_New(NULL);
        if(pbEPROCESS && cbEPROCESS) {
            pProcess->win.EPROCESS.cb = min(sizeof(pProcess->win.EPROCESS.pb), cbEPROCESS);
            memcpy(pProcess->win.EPROCESS.pb, pbEPROCESS, pProcess->win.EPROCESS.cb);
        }
        // attach pre-existing static process info entry or create new
        pProcessOld = VmmProcessGet(dwPID);
        if(pProcessOld) {
            pProcess->pObPersistent = (PVMMOB_PROCESS_PERSISTENT)Ob_INCREF(pProcessOld->pObPersistent);
        } else {
            VmmProcessStatic_Initialize(pProcess);
        }
        Ob_DECREF(pProcessOld);
        pProcessOld = NULL;
    }
    // 5: Install new PID
    i = iStart = dwPID % VMM_PROCESSTABLE_ENTRIES_MAX;
    while(TRUE) {
        if(!ptNew->_M[i]) {
            ptNew->_M[i] = pProcess;
            ptNew->_iFLinkM[i] = ptNew->_iFLink;
            ptNew->_iFLink = (WORD)i;
            ptNew->c++;
            ptNew->cActive += (pProcess->dwState == 0) ? 1 : 0;
            Ob_DECREF(ptOld);
            Ob_DECREF(ptNew);
            // pProcess already "consumed" by table insertion so increase before returning ... 
            return (PVMM_PROCESS)Ob_INCREF(pProcess);
        }
        if(++i == VMM_PROCESSTABLE_ENTRIES_MAX) { i = 0; }
        if(i == iStart) { goto fail; }
    }
fail:
    Ob_DECREF(pProcess);
    Ob_DECREF(ptOld);
    Ob_DECREF(ptNew);
    return NULL;
}

/*
* Activate the pending, not yet active, processes added by VmmProcessCreateEntry.
* This will also clear any previous processes.
*/
VOID VmmProcessCreateFinish()
{
    PVMMOB_PROCESS_TABLE ptNew, ptOld;
    if(!(ptOld = ObContainer_GetOb(ctxVmm->pObCPROC))) {
        return;
    }
    if(!(ptNew = ObContainer_GetOb(ptOld->pObCNewPROC))) {
        Ob_DECREF(ptOld);
        return;
    }
    // Replace "existing" old process table with new.
    ObContainer_SetOb(ctxVmm->pObCPROC, ptNew);
    Ob_DECREF(ptNew);
    Ob_DECREF(ptOld);
}

/*
* Clear the TLB spider flag in all process objects.
*/
VOID VmmProcessTlbClear()
{
    PVMMOB_PROCESS_TABLE pt = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ctxVmm->pObCPROC);
    PVMM_PROCESS pProcess;
    WORD iProcess;
    if(!pt) { return; }
    iProcess = pt->_iFLink;
    pProcess = pt->_M[iProcess];
    while(pProcess) {
        pProcess->fTlbSpiderDone = FALSE;
        iProcess = pt->_iFLinkM[iProcess];
        pProcess = pt->_M[iProcess];
        if(!pProcess || iProcess == pt->_iFLink) { break; }
    }
    Ob_DECREF(pt);
}

/*
* List the PIDs and put them into the supplied table.
* -- pPIDs = user allocated DWORD array to receive result, or NULL.
* -- pcPIDs = ptr to number of DWORDs in pPIDs on entry - number of PIDs in system on exit.
* -- flags = 0 (recommended) or VMM_FLAG_PROCESS_SHOW_TERMINATED (_only_ if default setting in ctxVmm->flags should be overridden)
*/
VOID VmmProcessListPIDs(_Out_writes_opt_(*pcPIDs) PDWORD pPIDs, _Inout_ PSIZE_T pcPIDs, _In_ QWORD flags)
{
    PVMMOB_PROCESS_TABLE pt = (PVMMOB_PROCESS_TABLE)ObContainer_GetOb(ctxVmm->pObCPROC);
    BOOL fShowTerminated = ((flags | ctxVmm->flags) & VMM_FLAG_PROCESS_SHOW_TERMINATED);
    PVMM_PROCESS pProcess;
    WORD iProcess;
    DWORD i = 0;
    if(!pPIDs) {
        *pcPIDs = fShowTerminated ? pt->c : pt->cActive;
        Ob_DECREF(pt);
        return;
    }
    if(*pcPIDs < (fShowTerminated ? pt->c : pt->cActive)) {
        *pcPIDs = 0;
        Ob_DECREF(pt);
        return;
    }
    // copy all PIDs
    iProcess = pt->_iFLink;
    pProcess = pt->_M[iProcess];
    while(pProcess) {
        if(!pProcess->dwState || fShowTerminated) {
            *(pPIDs + i) = pProcess->dwPID;
            i++;
        }
        iProcess = pt->_iFLinkM[iProcess];
        pProcess = pt->_M[iProcess];
        if(!pProcess || (iProcess == pt->_iFLink)) { break; }
    }
    *pcPIDs = i;
    Ob_DECREF(pt);
}

/*
* Create the initial process table at startup.
*/
BOOL VmmProcessTableCreateInitial()
{
    PVMMOB_PROCESS_TABLE pt = (PVMMOB_PROCESS_TABLE)Ob_Alloc(OB_TAG_VMM_PROCESSTABLE, LMEM_ZEROINIT, sizeof(VMMOB_PROCESS_TABLE), VmmProcessTable_CloseObCallback, NULL);
    if(!pt) { return FALSE; }
    pt->pObCNewPROC = ObContainer_New(NULL);
    ctxVmm->pObCPROC = ObContainer_New(pt);
    Ob_DECREF(pt);
    return TRUE;
}

// ----------------------------------------------------------------------------
// WORK (THREAD POOL) API:
// The 'Work' thread pool contain by default 32 threads which is waiting to
// receive work scheduled by calling the VmmWork function.
// ----------------------------------------------------------------------------

typedef struct tdVMMWORK_UNIT {
    LPTHREAD_START_ROUTINE pfn;     // function to call
    PVOID ctx;                      // optional function parameter
    HANDLE hEventFinish;            // optional event to set when upon work completion
} VMMWORK_UNIT, *PVMMWORK_UNIT;

typedef struct tdVMMWORK_THREAD_CONTEXT {
    HANDLE hEventWakeup;
    HANDLE hThread;
} VMMWORK_THREAD_CONTEXT, *PVMMWORK_THREAD_CONTEXT;

DWORD VmmWork_MainWorkerLoop_ThreadProc(PVMMWORK_THREAD_CONTEXT ctx)
{
    PVMMWORK_UNIT pu;
    while(ctxVmm->Work.fEnabled) {
        if((pu = (PVMMWORK_UNIT)ObSet_Pop(ctxVmm->Work.psUnit))) {
            pu->pfn(pu->ctx);
            if(pu->hEventFinish) {
                SetEvent(pu->hEventFinish);
            }
            LocalFree(pu);
        } else {
            ResetEvent(ctx->hEventWakeup);
            ObSet_Push(ctxVmm->Work.psThreadAvail, (QWORD)ctx);
            WaitForSingleObject(ctx->hEventWakeup, INFINITE);
        }
    }
    ObSet_Remove(ctxVmm->Work.psThreadAll, (QWORD)ctx);
    CloseHandle(ctx->hEventWakeup);
    CloseHandle(ctx->hThread);
    LocalFree(ctx);
    return 1;
}

VOID VmmWork_Initialize()
{
    PVMMWORK_THREAD_CONTEXT p;
    ctxVmm->Work.fEnabled = TRUE;
    ctxVmm->Work.psUnit = ObSet_New();
    ctxVmm->Work.psThreadAll = ObSet_New();
    ctxVmm->Work.psThreadAvail = ObSet_New();
    while(ObSet_Size(ctxVmm->Work.psThreadAll) < VMM_WORK_THREADPOOL_NUM_THREADS) {
        if((p = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMWORK_THREAD_CONTEXT)))) {
            p->hEventWakeup = CreateEvent(NULL, TRUE, FALSE, NULL);
            p->hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)VmmWork_MainWorkerLoop_ThreadProc, p, 0, NULL);
            ObSet_Push(ctxVmm->Work.psThreadAll, (QWORD)p);
        }
    }
}

VOID VmmWork_Close()
{
    PVMMWORK_UNIT pu;
    PVMMWORK_THREAD_CONTEXT pt = NULL;
    ctxVmm->Work.fEnabled = FALSE;
    while(ObSet_Size(ctxVmm->Work.psThreadAll)) {
        while((pt = (PVMMWORK_THREAD_CONTEXT)ObSet_GetNext(ctxVmm->Work.psThreadAll, (QWORD)pt))) {
            SetEvent(pt->hEventWakeup);
        }
        SwitchToThread();
    }
    while((pu = (PVMMWORK_UNIT)ObSet_Pop(ctxVmm->Work.psUnit))) {
        if(pu->hEventFinish) {
            SetEvent(pu->hEventFinish);
        }
        LocalFree(pu);
    }
    Ob_DECREF_NULL(&ctxVmm->Work.psUnit);
    Ob_DECREF_NULL(&ctxVmm->Work.psThreadAll);
    Ob_DECREF_NULL(&ctxVmm->Work.psThreadAvail);
}

VOID VmmWork(_In_ LPTHREAD_START_ROUTINE pfn, _In_opt_ PVOID ctx, _In_opt_ HANDLE hEventFinish)
{
    PVMMWORK_UNIT pu;
    PVMMWORK_THREAD_CONTEXT pt;
    if((pu = LocalAlloc(0, sizeof(VMMWORK_UNIT)))) {
        pu->pfn = pfn;
        pu->ctx = ctx;
        pu->hEventFinish = hEventFinish;
        ObSet_Push(ctxVmm->Work.psUnit, (QWORD)pu);
        if((pt = (PVMMWORK_THREAD_CONTEXT)ObSet_Pop(ctxVmm->Work.psThreadAvail))) {
            SetEvent(pt->hEventWakeup);
        }
    }
}

// ----------------------------------------------------------------------------
// PROCESS PARALLELIZATION FUNCTIONALITY:
// ----------------------------------------------------------------------------

typedef struct tdVMM_PROCESS_ACTION_FOREACH {
    HANDLE hEventFinish;
    VOID(*pfnAction)(_In_ PVMM_PROCESS pProcess, _In_ PVOID ctx);
    PVOID ctxAction;
    DWORD cRemainingWork;       // set to dwPIDs count on entry and decremented as-goes - when zero FinishEvent is set.
    DWORD iPID;                 // set to dwPIDs count on entry and decremented as-goes
    DWORD dwPIDs[];
} VMM_PROCESS_ACTION_FOREACH, *PVMM_PROCESS_ACTION_FOREACH;

DWORD VmmProcessActionForeachParallel_ThreadProc(PVMM_PROCESS_ACTION_FOREACH ctx)
{
    PVMM_PROCESS pObProcess = VmmProcessGet(ctx->dwPIDs[InterlockedDecrement(&ctx->iPID)]);
    if(pObProcess) {
        ctx->pfnAction(pObProcess, ctx->ctxAction);
        Ob_DECREF(pObProcess);
    }
    if(0 == InterlockedDecrement(&ctx->cRemainingWork)) {
        SetEvent(ctx->hEventFinish);
    }
    return 1;
}

BOOL VmmProcessActionForeachParallel_CriteriaActiveOnly(_In_ PVMM_PROCESS pProcess, _In_opt_ PVOID ctx)
{
    return pProcess->dwState == 0;
}

VOID VmmProcessActionForeachParallel(_In_opt_ PVOID ctxAction, _In_opt_ BOOL(*pfnCriteria)(_In_ PVMM_PROCESS pProcess, _In_opt_ PVOID ctx), _In_ VOID(*pfnAction)(_In_ PVMM_PROCESS pProcess, _In_opt_ PVOID ctx))
{
    DWORD i, cProcess;
    PVMM_PROCESS pObProcess = NULL;
    POB_SET pObProcessSelectedSet = NULL;
    PVMM_PROCESS_ACTION_FOREACH ctx = NULL;
    // 1: select processes to queue using criteria function
    if(!(pObProcessSelectedSet = ObSet_New())) { goto fail; }
    while(pObProcess = VmmProcessGetNext(pObProcess, VMM_FLAG_PROCESS_SHOW_TERMINATED)) {
        if(!pfnCriteria || pfnCriteria(pObProcess, ctx)) {
            ObSet_Push(pObProcessSelectedSet, pObProcess->dwPID);
        }
    }
    if(!(cProcess = ObSet_Size(pObProcessSelectedSet))) { goto fail; }
    // 2: set up context for worker function
    if(!(ctx = LocalAlloc(LMEM_ZEROINIT, sizeof(VMM_PROCESS_ACTION_FOREACH) + cProcess * sizeof(DWORD)))) { goto fail; }
    if(!(ctx->hEventFinish = CreateEvent(NULL, TRUE, FALSE, NULL))) { goto fail; }
    ctx->pfnAction = pfnAction;
    ctx->ctxAction = ctxAction;
    ctx->cRemainingWork = cProcess;
    ctx->iPID = cProcess;
    for(i = 0; i < cProcess; i++) {
        ctx->dwPIDs[i] = (DWORD)ObSet_Pop(pObProcessSelectedSet);
    }
    // 3: parallelize onto worker threads and wait for completion
    for(i = 0; i < cProcess; i++) {
        VmmWork(VmmProcessActionForeachParallel_ThreadProc, ctx, NULL);
    }
    WaitForSingleObject(ctx->hEventFinish, INFINITE);
    BOOL DEBUG = ctxVmm->f32;
fail:
    Ob_DECREF(pObProcessSelectedSet);
    if(ctx) {
        if(ctx->hEventFinish) {
            CloseHandle(ctx->hEventFinish);
        }
        LocalFree(ctx);
    }
}

// ----------------------------------------------------------------------------
// INTERNAL VMMU FUNCTIONALITY: VIRTUAL MEMORY ACCESS.
// ----------------------------------------------------------------------------

VOID VmmWriteScatterPhysical(_Inout_ PPMEM_SCATTER ppMEMsPhys, _In_ DWORD cpMEMsPhys)
{
    DWORD i;
    PMEM_SCATTER pMEM;
    LcWriteScatter(ctxMain->hLC, cpMEMsPhys, ppMEMsPhys);
    for(i = 0; i < cpMEMsPhys; i++) {
        pMEM = ppMEMsPhys[i];
        InterlockedIncrement64(&ctxVmm->stat.cPhysWrite);
        if(pMEM->f && MEM_SCATTER_ADDR_ISVALID(pMEM)) {
            VmmCacheInvalidate(pMEM->qwA & ~0xfff);
        }
    }
}

VOID VmmWriteScatterVirtual(_In_ PVMM_PROCESS pProcess, _Inout_ PPMEM_SCATTER ppMEMsVirt, _In_ DWORD cpMEMsVirt)
{
    DWORD i;
    QWORD qwPA_PTE = 0, qwPagedPA = 0;
    PMEM_SCATTER pMEM;
    for(i = 0; i < cpMEMsVirt; i++) {
        pMEM = ppMEMsVirt[i];
        MEM_SCATTER_STACK_PUSH(pMEM, pMEM->qwA);
        if(pMEM->f || (pMEM->qwA == -1)) {
            pMEM->qwA = -1;
            continue;
        }
        if(VmmVirt2Phys(pProcess, pMEM->qwA, &qwPA_PTE)) {
            pMEM->qwA = qwPA_PTE;
            continue;
        }
        // paged "read" also translate virtual -> physical for some
        // types of paged memory such as transition and prototype.
        ctxVmm->fnMemoryModel.pfnPagedRead(pProcess, pMEM->qwA, qwPA_PTE, NULL, &qwPagedPA, 0);
        pMEM->qwA = qwPagedPA ? qwPagedPA : -1;
    }
    VmmWriteScatterPhysical(ppMEMsVirt, cpMEMsVirt);
    for(i = 0; i < cpMEMsVirt; i++) {
        ppMEMsVirt[i]->qwA = MEM_SCATTER_STACK_POP(ppMEMsVirt[i]);
    }
}

VOID VmmReadScatterPhysical(_Inout_ PPMEM_SCATTER ppMEMsPhys, _In_ DWORD cpMEMsPhys, _In_ QWORD flags)
{
    QWORD tp;   // 0 = normal, 1 = already read, 2 = cache hit, 3 = speculative read
    BOOL fCache;
    PMEM_SCATTER pMEM;
    DWORD i, c, cSpeculative;
    PVMMOB_MEM pObCacheEntry, pObReservedMEM;
    PMEM_SCATTER ppMEMsSpeculative[0x18];
    PVMMOB_MEM ppObCacheSpeculative[0x18];
    fCache = !(VMM_FLAG_NOCACHE & (flags | ctxVmm->flags));
    // 1: cache read
    if(fCache) {
        c = 0, cSpeculative = 0;
        for(i = 0; i < cpMEMsPhys; i++) {
            pMEM = ppMEMsPhys[i];
            if(pMEM->f) {
                // already valid -> skip
                MEM_SCATTER_STACK_PUSH(pMEM, 3);    // 3: already finished
                c++;
                continue;
            }
            // retrieve from cache (if found)
            if((pMEM->cb == 0x1000) && (pObCacheEntry = VmmCacheGet(VMM_CACHE_TAG_PHYS, pMEM->qwA))) {
                // in cache - copy data into requester and set as completed!
                MEM_SCATTER_STACK_PUSH(pMEM, 2);    // 2: cache read
                pMEM->f = TRUE;
                memcpy(pMEM->pb, pObCacheEntry->pb, 0x1000);
                Ob_DECREF(pObCacheEntry);
                InterlockedIncrement64(&ctxVmm->stat.cPhysCacheHit);
                c++;
                continue;
            }
            MEM_SCATTER_STACK_PUSH(pMEM, 1);        // 1: normal read
            // add to potential speculative read map if read is small enough...
            if(cSpeculative < 0x18) {
                ppMEMsSpeculative[cSpeculative++] = pMEM;
            }
        }
        // all found in cache _OR_ only cached reads allowed -> restore mem stack and return!
        if((c == cpMEMsPhys) || (VMM_FLAG_FORCECACHE_READ & flags)) {
            for(i = 0; i < cpMEMsPhys; i++) {
                MEM_SCATTER_STACK_POP(ppMEMsPhys[i]);
            }
            return;
        }
    }
    // 2: speculative future read if negligible performance loss
    if(fCache && cSpeculative && (cSpeculative < 0x18)) {
        for(i = 0; i < cpMEMsPhys; i++) {
            pMEM = ppMEMsPhys[i];
            if(1 != MEM_SCATTER_STACK_PEEK(pMEM, 1)) {
                MEM_SCATTER_STACK_POP(pMEM);
            }
        }
        while(cSpeculative < 0x18) {
            if((ppObCacheSpeculative[cSpeculative] = VmmCacheReserve(VMM_CACHE_TAG_PHYS))) {
                pMEM = ppMEMsSpeculative[cSpeculative] = &ppObCacheSpeculative[cSpeculative]->h;
                MEM_SCATTER_STACK_PUSH(pMEM, 4);
                pMEM->f = FALSE;
                pMEM->qwA = ((QWORD)ppMEMsSpeculative[cSpeculative - 1]->qwA & ~0xfff) + 0x1000;
                cSpeculative++;
            }
        }
        ppMEMsPhys = ppMEMsSpeculative;
        cpMEMsPhys = cSpeculative;
    }
    // 3: read!
    LcReadScatter(ctxMain->hLC, cpMEMsPhys, ppMEMsPhys);
    // 4: statistics and read fail zero fixups (if required)
    for(i = 0; i < cpMEMsPhys; i++) {
        pMEM = ppMEMsPhys[i];
        if(pMEM->f) {
            // success
            InterlockedIncrement64(&ctxVmm->stat.cPhysReadSuccess);
        } else {
            // fail
            InterlockedIncrement64(&ctxVmm->stat.cPhysReadFail);
            if((flags & VMM_FLAG_ZEROPAD_ON_FAIL) && (pMEM->qwA < ctxMain->dev.paMax)) {
                ZeroMemory(pMEM->pb, pMEM->cb);
                pMEM->f = TRUE;
            }
        }
    }
    // 5: cache put
    if(fCache) {
        for(i = 0; i < cpMEMsPhys; i++) {
            pMEM = ppMEMsPhys[i];
            tp = MEM_SCATTER_STACK_POP(pMEM);
            if(!(VMM_FLAG_NOCACHEPUT & flags)) {
                if(tp == 4) {   // 4 == speculative & backed by cache reserved
                    VmmCacheReserveReturn(ppObCacheSpeculative[i]);
                }
                if((tp == 1) && pMEM->f) { // 1 = normal read
                    if((pObReservedMEM = VmmCacheReserve(VMM_CACHE_TAG_PHYS))) {
                        pObReservedMEM->h.f = TRUE;
                        pObReservedMEM->h.qwA = pMEM->qwA;
                        memcpy(pObReservedMEM->h.pb, pMEM->pb, 0x1000);
                        VmmCacheReserveReturn(pObReservedMEM);
                    }
                }
            }
        }
    }
}

VOID VmmReadScatterVirtual(_In_ PVMM_PROCESS pProcess, _Inout_updates_(cpMEMsVirt) PPMEM_SCATTER ppMEMsVirt, _In_ DWORD cpMEMsVirt, _In_ QWORD flags)
{
    // NB! the buffers pIoPA / ppMEMsPhys are used for both:
    //     - physical memory (grows from 0 upwards)
    //     - paged memory (grows from top downwards).
    BOOL fVirt2Phys;
    DWORD i = 0, iVA, iPA;
    QWORD qwPA, qwPagedPA = 0;
    BYTE pbBufferSmall[0x20 * (sizeof(MEM_SCATTER) + sizeof(PMEM_SCATTER))];
    PBYTE pbBufferMEMs, pbBufferLarge = NULL;
    PMEM_SCATTER pIoPA, pIoVA;
    PPMEM_SCATTER ppMEMsPhys = NULL;
    BOOL fPaging = !(VMM_FLAG_NOPAGING & (flags | ctxVmm->flags));
    BOOL fAltAddrPte = VMM_FLAG_ALTADDR_VA_PTE & flags;
    BOOL fZeropadOnFail = VMM_FLAG_ZEROPAD_ON_FAIL & (flags | ctxVmm->flags);
    // 1: allocate / set up buffers (if needed)
    if(cpMEMsVirt < 0x20) {
        ZeroMemory(pbBufferSmall, sizeof(pbBufferSmall));
        ppMEMsPhys = (PPMEM_SCATTER)pbBufferSmall;
        pbBufferMEMs = pbBufferSmall + cpMEMsVirt * sizeof(PMEM_SCATTER);
    } else {
        if(!(pbBufferLarge = LocalAlloc(LMEM_ZEROINIT, cpMEMsVirt * (sizeof(MEM_SCATTER) + sizeof(PMEM_SCATTER))))) { return; }
        ppMEMsPhys = (PPMEM_SCATTER)pbBufferLarge;
        pbBufferMEMs = pbBufferLarge + cpMEMsVirt * sizeof(PMEM_SCATTER);
    }
    // 2: translate virt2phys
    for(iVA = 0, iPA = 0; iVA < cpMEMsVirt; iVA++) {
        pIoVA = ppMEMsVirt[iVA];
        // MEMORY READ ALREADY COMPLETED
        if(pIoVA->f || (pIoVA->qwA == 0) || (pIoVA->qwA == -1)) {
            if(!pIoVA->f && fZeropadOnFail) {
                ZeroMemory(pIoVA->pb, pIoVA->cb);
            }
            continue;
        }
        // PHYSICAL MEMORY
        qwPA = 0;
        fVirt2Phys = !fAltAddrPte && VmmVirt2Phys(pProcess, pIoVA->qwA, &qwPA);
        // PAGED MEMORY
        if(!fVirt2Phys && fPaging && (pIoVA->cb == 0x1000) && ctxVmm->fnMemoryModel.pfnPagedRead) {
            if(ctxVmm->fnMemoryModel.pfnPagedRead(pProcess, (fAltAddrPte ? 0 : pIoVA->qwA), (fAltAddrPte ? pIoVA->qwA : qwPA), pIoVA->pb, &qwPagedPA, flags)) {
                continue;
            }
            if(qwPagedPA) {
                qwPA = qwPagedPA;
                fVirt2Phys = TRUE;
            }
        }
        if(!fVirt2Phys) {   // NO TRANSLATION MEMORY / FAILED PAGED MEMORY
            if(fZeropadOnFail) {
                ZeroMemory(pIoVA->pb, pIoVA->cb);
            }
            continue;
        }
        // PHYS MEMORY
        pIoPA = ppMEMsPhys[iPA] = (PMEM_SCATTER)pbBufferMEMs + iPA;
        iPA++;
        pIoPA->version = MEM_SCATTER_VERSION;
        pIoPA->qwA = qwPA;
        pIoPA->cb = 0x1000;
        pIoPA->pb = pIoVA->pb;
        pIoPA->f = FALSE;
        MEM_SCATTER_STACK_PUSH(pIoPA, (QWORD)pIoVA);
    }
    // 3: read and check result
    if(iPA) {
        VmmReadScatterPhysical(ppMEMsPhys, iPA, flags);
        while(iPA > 0) {
            iPA--;
            ((PMEM_SCATTER)MEM_SCATTER_STACK_POP(ppMEMsPhys[iPA]))->f = ppMEMsPhys[iPA]->f;
        }
    }
    LocalFree(pbBufferLarge);
}

/*
* Retrieve information of the physical2virtual address translation for the
* supplied process. This function may take time on larger address spaces -
* such as the kernel adderss space due to extensive page walking. If a new
* address is to be used please supply it in paTarget. If paTarget == 0 then
* a previously stored address will be used.
* It's not possible to use this function to retrieve multiple targeted
* addresses in parallell.
* -- CALLER DECREF: return
* -- pProcess
* -- paTarget = targeted physical address (or 0 if use previously saved).
* -- return
*/
PVMMOB_PHYS2VIRT_INFORMATION VmmPhys2VirtGetInformation(_In_ PVMM_PROCESS pProcess, _In_ QWORD paTarget)
{
    PVMMOB_PHYS2VIRT_INFORMATION pObP2V = NULL;
    if(paTarget) {
        pProcess->pObPersistent->Plugin.paPhys2Virt = paTarget;
    } else {
        paTarget = pProcess->pObPersistent->Plugin.paPhys2Virt;
    }
    pObP2V = ObContainer_GetOb(pProcess->Plugin.pObCPhys2Virt);
    if(paTarget && (!pObP2V || (pObP2V->paTarget != paTarget))) {
        Ob_DECREF_NULL(&pObP2V);
        EnterCriticalSection(&pProcess->LockUpdate);
        pObP2V = ObContainer_GetOb(pProcess->Plugin.pObCPhys2Virt);
        if(paTarget && (!pObP2V || (pObP2V->paTarget != paTarget))) {
            Ob_DECREF_NULL(&pObP2V);
            pObP2V = Ob_Alloc('PAVA', LMEM_ZEROINIT, sizeof(VMMOB_PHYS2VIRT_INFORMATION), NULL, NULL);
            pObP2V->paTarget = paTarget;
            pObP2V->dwPID = pProcess->dwPID;
            if(ctxVmm->fnMemoryModel.pfnPhys2VirtGetInformation) {
                ctxVmm->fnMemoryModel.pfnPhys2VirtGetInformation(pProcess, pObP2V);
                ObContainer_SetOb(pProcess->Plugin.pObCPhys2Virt, pObP2V);
            }
        }
        LeaveCriticalSection(&pProcess->LockUpdate);
    }
    if(!pObP2V) {
        EnterCriticalSection(&pProcess->LockUpdate);
        pObP2V = ObContainer_GetOb(pProcess->Plugin.pObCPhys2Virt);
        if(!pObP2V) {
            pObP2V = Ob_Alloc('PAVA', LMEM_ZEROINIT, sizeof(VMMOB_PHYS2VIRT_INFORMATION), NULL, NULL);
            pObP2V->dwPID = pProcess->dwPID;
            ObContainer_SetOb(pProcess->Plugin.pObCPhys2Virt, pObP2V);
        }
        LeaveCriticalSection(&pProcess->LockUpdate);
    }
    return pObP2V;
}

// ----------------------------------------------------------------------------
// PUBLICALLY VISIBLE FUNCTIONALITY RELATED TO VMMU.
// ----------------------------------------------------------------------------

VOID VmmClose()
{
    if(!ctxVmm) { return; }
    if(ctxVmm->PluginManager.FLink) { PluginManager_Close(); }
    VmmWork_Close();
    VmmWinObj_Close();
    VmmWinReg_Close();
    PDB_Close();
    Ob_DECREF_NULL(&ctxVmm->pObVfsDumpContext);
    Ob_DECREF_NULL(&ctxVmm->pObPfnContext);
    Ob_DECREF_NULL(&ctxVmm->pObCPROC);
    if(ctxVmm->fnMemoryModel.pfnClose) {
        ctxVmm->fnMemoryModel.pfnClose();
    }
    MmWin_PagingClose();
    VmmCache2Close(VMM_CACHE_TAG_PHYS);
    VmmCache2Close(VMM_CACHE_TAG_TLB);
    VmmCache2Close(VMM_CACHE_TAG_PAGING);
    Ob_DECREF_NULL(&ctxVmm->Cache.PAGING_FAILED);
    Ob_DECREF_NULL(&ctxVmm->Cache.pmPrototypePte);
    Ob_DECREF_NULL(&ctxVmm->pObCMapPhysMem);
    Ob_DECREF_NULL(&ctxVmm->pObCMapUser);
    Ob_DECREF_NULL(&ctxVmm->pObCMapNet);
    Ob_DECREF_NULL(&ctxVmm->pObCCachePrefetchEPROCESS);
    Ob_DECREF_NULL(&ctxVmm->pObCCachePrefetchRegistry);
    DeleteCriticalSection(&ctxVmm->TcpIp.LockUpdate);
    DeleteCriticalSection(&ctxVmm->LockMaster);
    DeleteCriticalSection(&ctxVmm->LockPlugin);
    DeleteCriticalSection(&ctxVmm->LockUpdateMap);
    DeleteCriticalSection(&ctxVmm->LockUpdateModule);
    LocalFree(ctxVmm->ObjectTypeTable.wszMultiText);
    LocalFree(ctxVmm);
    ctxVmm = NULL;
}

VOID VmmWriteEx(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwA, _In_ PBYTE pb, _In_ DWORD cb, _Out_opt_ PDWORD pcbWrite)
{
    DWORD i = 0, oA = 0, cbWrite = 0, cbP, cMEMs;
    PBYTE pbBuffer;
    PMEM_SCATTER pMEM, pMEMs, *ppMEMs;
    if(pcbWrite) { *pcbWrite = 0; }
    // allocate
    cMEMs = (DWORD)(((qwA & 0xfff) + cb + 0xfff) >> 12);
    if(!(pbBuffer = (PBYTE)LocalAlloc(LMEM_ZEROINIT, cMEMs * (sizeof(MEM_SCATTER) + sizeof(PMEM_SCATTER))))) { return; }
    pMEMs = (PMEM_SCATTER)pbBuffer;
    ppMEMs = (PPMEM_SCATTER)(pbBuffer + cMEMs * sizeof(MEM_SCATTER));
    // prepare pages
    while(oA < cb) {
        cbP = 0x1000 - ((qwA + oA) & 0xfff);
        cbP = min(cbP, cb - oA);
        ppMEMs[i++] = pMEM = pMEMs + i;
        pMEM->version = MEM_SCATTER_VERSION;
        pMEM->qwA = qwA + oA;
        pMEM->cb = cbP;
        pMEM->pb = pb + oA;
        oA += cbP;
    }
    // write and count result
    if(pProcess) {
        VmmWriteScatterVirtual(pProcess, ppMEMs, cMEMs);
    } else {
        VmmWriteScatterPhysical(ppMEMs, cMEMs);
    }
    if(pcbWrite) {
        for(i = 0; i < cMEMs; i++) {
            if(pMEMs[i].f) {
                cbWrite += pMEMs[i].cb;
            }
        }
        *pcbWrite = cbWrite;
    }
    LocalFree(pbBuffer);
}

BOOL VmmWrite(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwA, _In_reads_(cb) PBYTE pb, _In_ DWORD cb)
{
    DWORD cbWrite;
    VmmWriteEx(pProcess, qwA, pb, cb, &cbWrite);
    return (cbWrite == cb);
}

VOID VmmReadEx(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwA, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb, _Out_opt_ PDWORD pcbReadOpt, _In_ QWORD flags)
{
    DWORD cbP, cMEMs, cbRead = 0;
    PBYTE pbBuffer;
    PMEM_SCATTER pMEMs, *ppMEMs;
    QWORD i, oA;
    if(pcbReadOpt) { *pcbReadOpt = 0; }
    if(!cb) { return; }
    cMEMs = (DWORD)(((qwA & 0xfff) + cb + 0xfff) >> 12);
    pbBuffer = (PBYTE)LocalAlloc(LMEM_ZEROINIT, 0x2000 + cMEMs * (sizeof(MEM_SCATTER) + sizeof(PMEM_SCATTER)));
    if(!pbBuffer) {
        ZeroMemory(pb, cb);
        return;
    }
    pMEMs = (PMEM_SCATTER)(pbBuffer + 0x2000);
    ppMEMs = (PPMEM_SCATTER)(pbBuffer + 0x2000 + cMEMs * sizeof(MEM_SCATTER));
    oA = qwA & 0xfff;
    // prepare "middle" pages
    for(i = 0; i < cMEMs; i++) {
        ppMEMs[i] = &pMEMs[i];
        pMEMs[i].version = MEM_SCATTER_VERSION;
        pMEMs[i].qwA = qwA - oA + (i << 12);
        pMEMs[i].cb = 0x1000;
        pMEMs[i].pb = pb - oA + (i << 12);
    }
    // fixup "first/last" pages
    pMEMs[0].pb = pbBuffer;
    if(cMEMs > 1) {
        pMEMs[cMEMs - 1].pb = pbBuffer + 0x1000;
    }
    // Read VMM and handle result
    if(pProcess) {
        VmmReadScatterVirtual(pProcess, ppMEMs, cMEMs, flags);
    } else {
        VmmReadScatterPhysical(ppMEMs, cMEMs, flags);
    }
    for(i = 0; i < cMEMs; i++) {
        if(pMEMs[i].f) {
            cbRead += 0x1000;
        } else {
            ZeroMemory(pMEMs[i].pb, 0x1000);
        }
    }
    cbRead -= pMEMs[0].f ? 0x1000 : 0;                             // adjust byte count for first page (if needed)
    cbRead -= ((cMEMs > 1) && pMEMs[cMEMs - 1].f) ? 0x1000 : 0;    // adjust byte count for last page (if needed)
    // Handle first page
    cbP = (DWORD)min(cb, 0x1000 - oA);
    if(pMEMs[0].f) {
        memcpy(pb, pMEMs[0].pb + oA, cbP);
        cbRead += cbP;
    } else {
        ZeroMemory(pb, cbP);
    }
    // Handle last page
    if(cMEMs > 1) {
        cbP = (((qwA + cb) & 0xfff) ? ((qwA + cb) & 0xfff) : 0x1000);
        if(pMEMs[cMEMs - 1].f) {
            memcpy(pb + ((QWORD)cMEMs << 12) - oA - 0x1000, pMEMs[cMEMs - 1].pb, cbP);
            cbRead += cbP;
        } else {
            ZeroMemory(pb + ((QWORD)cMEMs << 12) - oA - 0x1000, cbP);
        }
    }
    if(pcbReadOpt) { *pcbReadOpt = cbRead; }
    LocalFree(pbBuffer);
}

#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#define STATUS_END_OF_FILE               ((NTSTATUS)0xC0000011L)

NTSTATUS VmmReadAsFile(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwMemoryAddress, _In_ QWORD cbMemorySize, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbRead, _In_ QWORD cbOffset)
{
    QWORD cbMax;
    if(cbMemorySize <= cbOffset) {
        *pcbRead = 0;
        return STATUS_END_OF_FILE;
    }
    cbMax = min(qwMemoryAddress + cbMemorySize, (qwMemoryAddress + cb + cbOffset)) - (qwMemoryAddress + cbOffset);   // min(entry_top_addr, request_top_addr) - request_start_addr
    *pcbRead = (DWORD)min(cb, cbMax);
    if(!*pcbRead) {
        return STATUS_END_OF_FILE;
    }
    VmmReadEx(pProcess, qwMemoryAddress + cbOffset, pb, *pcbRead, NULL, VMM_FLAG_ZEROPAD_ON_FAIL);
    return STATUS_SUCCESS;
}

NTSTATUS VmmWriteAsFile(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwMemoryAddress, _In_ QWORD cbMemorySize, _In_reads_(cb) PBYTE pb, _In_ DWORD cb, _Out_ PDWORD pcbWrite, _In_ QWORD cbOffset)
{
    QWORD cbMax;
    if(cbMemorySize <= cbOffset) {
        *pcbWrite = 0;
        return STATUS_END_OF_FILE;
    }
    cbMax = min(qwMemoryAddress + cbMemorySize, (qwMemoryAddress + cb + cbOffset)) - (qwMemoryAddress + cbOffset);   // min(entry_top_addr, request_top_addr) - request_start_addr
    *pcbWrite = (DWORD)min(cb, cbMax);
    if(!*pcbWrite) {
        return STATUS_END_OF_FILE;
    }
    VmmWriteEx(pProcess, qwMemoryAddress + cbOffset, pb, *pcbWrite, NULL);
    return STATUS_SUCCESS;
}

_Success_(return)
BOOL VmmReadAlloc(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwA, _Out_ PBYTE *ppb, _In_ DWORD cb, _In_ QWORD flags)
{
    PBYTE pb;
    if(!(pb = LocalAlloc(0, cb + 2ULL))) { return FALSE; }
    if(!VmmRead2(pProcess, qwA, pb, cb, flags)) {
        LocalFree(pb);
        return FALSE;
    }
    pb[cb] = 0;
    pb[cb + 1] = 0;
    *ppb = pb;
    return TRUE;
}

_Success_(return)
BOOL VmmReadAllocUnicodeString_Size(_In_ PVMM_PROCESS pProcess, _In_ BOOL f32, _In_ QWORD flags, _In_ QWORD vaUS, _Out_ PQWORD pvaStr, _Out_ PWORD pcbStr)
{
    BYTE pb[16];
    DWORD cbRead;
    VmmReadEx(pProcess, vaUS, pb, (f32 ? 8 : 16), &cbRead, flags);
    return
        (cbRead == (f32 ? 8 : 16)) &&                               // read ok
        (*(PWORD)pb <= *(PWORD)(pb + 2)) &&                         // size max >= size
        (*pcbStr = *(PWORD)pb) &&                                   // size != 0
        (*pcbStr > 1) &&                                            // size > 1
        (*pvaStr = f32 ? *(PDWORD)(pb + 4) : *(PQWORD)(pb + 8)) &&  // string address != 0
        !(*pvaStr & (f32 ? 3 : 7));                                 // non alignment
}

_Success_(return)
BOOL VmmReadAllocUnicodeString(_In_ PVMM_PROCESS pProcess, _In_ BOOL f32, _In_ QWORD flags, _In_ QWORD vaUS, _In_ DWORD cchMax, _Out_opt_ LPWSTR *pwsz, _Out_opt_ PDWORD pcch)
{
    WORD cbStr;
    QWORD vaStr;
    if(pcch) { *pcch = 0; }
    if(pwsz) { *pwsz = NULL; }
    if(VmmReadAllocUnicodeString_Size(pProcess, f32, 0, vaUS, &vaStr, &cbStr)) {
        if(cchMax && (cbStr > (cchMax << 1))) {
            cbStr = (WORD)(cchMax << 1);
        }
        if(!pwsz || VmmReadAlloc(pProcess, vaStr, (PBYTE *)pwsz, cbStr, flags)) {
            if(pcch) { *pcch = cbStr >> 1; }
            return TRUE;
        }
    }
    return FALSE;
}

_Success_(return)
BOOL VmmRead(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwA, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb)
{
    DWORD cbRead;
    VmmReadEx(pProcess, qwA, pb, cb, &cbRead, 0);
    return (cbRead == cb);
}

_Success_(return)
BOOL VmmRead2(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwA, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb, _In_ QWORD flags)
{
    DWORD cbRead;
    VmmReadEx(pProcess, qwA, pb, cb, &cbRead, flags);
    return (cbRead == cb);
}

_Success_(return)
BOOL VmmReadPage(_In_opt_ PVMM_PROCESS pProcess, _In_ QWORD qwA, _Out_writes_(4096) PBYTE pbPage)
{
    DWORD cb;
    VmmReadEx(pProcess, qwA, pbPage, 0x1000, &cb, 0);
    return cb == 0x1000;
}

VOID VmmInitializeMemoryModel(_In_ VMM_MEMORYMODEL_TP tp)
{
    switch(tp) {
        case VMM_MEMORYMODEL_X64:
            MmX64_Initialize();
            break;
        case VMM_MEMORYMODEL_X86PAE:
            MmX86PAE_Initialize();
            break;
        case VMM_MEMORYMODEL_X86:
            MmX86_Initialize();
            break;
        default:
            if(ctxVmm->fnMemoryModel.pfnClose) {
                ctxVmm->fnMemoryModel.pfnClose();
            }
    }
}

VOID VmmInitializeFunctions()
{
    HMODULE hNtDll = NULL;
    if((hNtDll = LoadLibraryA("ntdll.dll"))) {
        ctxVmm->fn.RtlDecompressBuffer = (VMMFN_RtlDecompressBuffer*)GetProcAddress(hNtDll, "RtlDecompressBuffer");
        FreeLibrary(hNtDll);
    }
}

BOOL VmmInitialize()
{
    // 1: allocate & initialize
    if(ctxVmm) { VmmClose(); }
    ctxVmm = (PVMM_CONTEXT)LocalAlloc(LMEM_ZEROINIT, sizeof(VMM_CONTEXT));
    if(!ctxVmm) { goto fail; }
    ctxVmm->hModuleVmm = GetModuleHandleA("vmm");
    // 2: CACHE INIT: Process Table
    if(!VmmProcessTableCreateInitial()) { goto fail; }
    // 3: CACHE INIT: Translation Lookaside Buffer (TLB) Cache Table
    VmmCache2Initialize(VMM_CACHE_TAG_TLB);
    if(!ctxVmm->Cache.TLB.fActive) { goto fail; }
    // 4: CACHE INIT: Physical Memory Cache Table
    VmmCache2Initialize(VMM_CACHE_TAG_PHYS);
    if(!ctxVmm->Cache.PHYS.fActive) { goto fail; }
    // 5: CACHE INIT: Paged Memory Cache Table
    VmmCache2Initialize(VMM_CACHE_TAG_PAGING);
    if(!ctxVmm->Cache.PAGING.fActive) { goto fail; }
    if(!(ctxVmm->Cache.PAGING_FAILED = ObSet_New())) { goto fail; }
    // 6: CACHE INIT: Prototype PTE Cache Map
    if(!(ctxVmm->Cache.pmPrototypePte = ObMap_New(OB_MAP_FLAGS_OBJECT_OB))) { goto fail; }
    // 7: WORKER THREADS INIT:
    VmmWork_Initialize();
    // 8: OTHER INIT:
    ctxVmm->pObCMapPhysMem = ObContainer_New(NULL);
    ctxVmm->pObCMapUser = ObContainer_New(NULL);
    ctxVmm->pObCMapNet = ObContainer_New(NULL);
    ctxVmm->pObCCachePrefetchEPROCESS = ObContainer_New(NULL);
    ctxVmm->pObCCachePrefetchRegistry = ObContainer_New(NULL);
    InitializeCriticalSection(&ctxVmm->LockMaster);
    InitializeCriticalSection(&ctxVmm->LockPlugin);
    InitializeCriticalSection(&ctxVmm->LockUpdateMap);
    InitializeCriticalSection(&ctxVmm->LockUpdateModule);
    InitializeCriticalSection(&ctxVmm->TcpIp.LockUpdate);
    VmmInitializeFunctions();
    return TRUE;
fail:
    VmmClose();
    return FALSE;
}
