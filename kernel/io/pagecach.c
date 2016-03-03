/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    pagecach.c

Abstract:

    This module implements support for the I/O page cache.

Author:

    Chris Stevens 11-Sep-2013

Environment:

    Kernel

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel/kernel.h>
#include "iop.h"
#include "pagecach.h"

//
// ---------------------------------------------------------------- Definitions
//

#define PAGE_CACHE_ALLOCATION_TAG 0x68436750 // 'hCgP'

//
// Define the percentage of the total system physical pages the page cache
// tries to keep free.
//

#define PAGE_CACHE_MEMORY_HEADROOM_PERCENT_TRIGGER 10
#define PAGE_CACHE_MEMORY_HEADROOM_PERCENT_RETREAT 15

//
// Define the target size in percent of total system physical memory that the
// page cache aims for. Below this size, paging out begins in addition to
// shrinking the page cache.
//

#define PAGE_CACHE_MINIMUM_MEMORY_TARGET_PERCENT 33

//
// Define the size in percent of total system physical memory that the page
// cache feels it's entitled to even when memory is tight. Performance simply
// suffers too much if the page cache shrinks to nothing.
//

#define PAGE_CACHE_MINIMUM_MEMORY_PERCENT 7

//
// Define the number of system virtual memory the page cache aims to keep free
// by unmapping page cache entries. This is stored in bytes. There are
// different values for system with small (<4GB) and large (64-bit) system
// virtual memory resources.
//

#define PAGE_CACHE_SMALL_VIRTUAL_HEADROOM_TRIGGER_BYTES (512 * _1MB)
#define PAGE_CACHE_SMALL_VIRTUAL_HEADROOM_RETREAT_BYTES (896 * _1MB)
#define PAGE_CACHE_LARGE_VIRTUAL_HEADROOM_TRIGGER_BYTES (1 * (UINTN)_1GB)
#define PAGE_CACHE_LARGE_VIRTUAL_HEADROOM_RETREAT_BYTES (3 * (UINTN)_1GB)

//
// Set this flag if the page cache entry contains dirty data.
//

#define PAGE_CACHE_ENTRY_FLAG_DIRTY 0x00000001

//
// Set this flag if the page cache entry owns the physical page it uses.
//

#define PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER 0x00000002

//
// Set this flag if the page cache entry is mapped. This needs to be a flag as
// opposed to just a check of the VA so that it can be managed atomically with
// the dirty flag, keeping the "mapped dirty page" count correct. This flag
// is meant to track whether or not a page is counted in the "mapped page
// count", and so it is not set on non page owners.
//

#define PAGE_CACHE_ENTRY_FLAG_MAPPED 0x00000004

//
// Define page cache debug flags.
//

#define PAGE_CACHE_DEBUG_INSERTION         0x00000001
#define PAGE_CACHE_DEBUG_LOOKUP            0x00000002
#define PAGE_CACHE_DEBUG_EVICTION          0x00000004
#define PAGE_CACHE_DEBUG_FLUSH             0x00000008
#define PAGE_CACHE_DEBUG_SIZE_MANAGEMENT   0x00000010
#define PAGE_CACHE_DEBUG_MAPPED_MANAGEMENT 0x00000020
#define PAGE_CACHE_DEBUG_DIRTY_LISTS       0x00000040

//
// Define parameters to help coalesce flushes.
//

#define PAGE_CACHE_FLUSH_MAX _128KB

//
// Define the maximum streak of clean pages the page cache encounters while
// flushing before breaking up a write.
//

#define PAGE_CACHE_FLUSH_MAX_CLEAN_STREAK 4

//
// Define the block expansion count for the page cache entry block allocator.
//

#define PAGE_CACHE_BLOCK_ALLOCATOR_EXPANSION_COUNT 0x40

//
// Define the maximum number of pages that can be used as the minimum number of
// free pages necessary to require page cache flushes to give up in favor of
// removing entries in a low memory situation.
//

#define PAGE_CACHE_LOW_MEMORY_CLEAN_PAGE_MAXIMUM 256

//
// Define the percentage of total physical pages that need to be free before
// the page cache stops cleaning entries to evict entries.
//

#define PAGE_CACHE_LOW_MEMORY_CLEAN_PAGE_MINIMUM_PERCENTAGE 10

//
// Define the portion of the page cache that should be dirty (maximum) as a
// shift.
//

#define PAGE_CACHE_MAX_DIRTY_SHIFT 1

//
// This defines the amount of time the page cache worker will delay until
// executing another cleaning. This allows writes to pool.
//

#define PAGE_CACHE_CLEAN_DELAY_MIN (5000 * MICROSECONDS_PER_MILLISECOND)

//
// --------------------------------------------------------------------- Macros
//

#define IS_IO_OBJECT_TYPE_LINKABLE(_IoObjectType)     \
    ((_IoObjectType == IoObjectRegularFile) ||        \
     (_IoObjectType == IoObjectSymbolicLink) ||       \
     (_IoObjectType == IoObjectSharedMemoryObject) || \
     (_IoObjectType == IoObjectBlockDevice))

//
// ------------------------------------------------------ Data Type Definitions
//

typedef enum _PAGE_CACHE_STATE {
    PageCacheStateInvalid,
    PageCacheStateClean,
    PageCacheStateDirty,
} PAGE_CACHE_STATE, *PPAGE_CACHE_STATE;

/*++

Structure Description:

    This structure defines a page cache entry.

Members:

    Node - Stores the Red-Black tree node information for this page cache
        entry.

    ListEntry - Stores this page cache entry's list entry in an LRU list, local
        list, or dirty list. This list entry is protected by the global page
        cache list lock.

    FileObject - Stores a pointer to the file object for the device or file to
        which the page cache entry belongs.

    Offset - Stores the offset into the file or device of the cached page.

    PhysicalAddress - Stores the physical address of the page containing the
        cached data.

    VirtualAddress - Stores the virtual address of the page containing the
        cached data.

    BackingEntry - Stores a pointer to a page cache entry that owns the
        physical page used by this page cache entry.

    ReferenceCount - Stores the number of references on this page cache entry.

    Flags - Stores a bitmask of page cache entry flags. See
        PAGE_CACHE_ENTRY_FLAG_* for definitions.

--*/

struct _PAGE_CACHE_ENTRY {
    RED_BLACK_TREE_NODE Node;
    LIST_ENTRY ListEntry;
    PFILE_OBJECT FileObject;
    ULONGLONG Offset;
    PHYSICAL_ADDRESS PhysicalAddress;
    PVOID VirtualAddress;
    PPAGE_CACHE_ENTRY BackingEntry;
    volatile ULONG ReferenceCount;
    volatile ULONG Flags;
};

//
// ----------------------------------------------- Internal Function Prototypes
//

PPAGE_CACHE_ENTRY
IopCreatePageCacheEntry (
    PFILE_OBJECT FileObject,
    PVOID VirtualAddress,
    PHYSICAL_ADDRESS PhysicalAddress,
    ULONGLONG Offset
    );

VOID
IopDestroyPageCacheEntries (
    PLIST_ENTRY ListHead
    );

VOID
IopDestroyPageCacheEntry (
    PPAGE_CACHE_ENTRY PageCacheEntry
    );

VOID
IopInsertPageCacheEntry (
    PPAGE_CACHE_ENTRY NewEntry,
    PPAGE_CACHE_ENTRY LinkEntry
    );

PPAGE_CACHE_ENTRY
IopLookupPageCacheEntryHelper (
    PFILE_OBJECT FileObject,
    ULONGLONG Offset
    );

VOID
IopPageCacheThread (
    PVOID Parameter
    );

KSTATUS
IopFlushPageCacheBuffer (
    PIO_BUFFER FlushBuffer,
    UINTN FlushSize,
    ULONG Flags
    );

VOID
IopTrimRemovalPageCacheList (
    VOID
    );

VOID
IopRemovePageCacheEntriesFromList (
    PLIST_ENTRY PageCacheListHead,
    PLIST_ENTRY DestroyListHead,
    BOOL TimidEffort,
    PUINTN TargetRemoveCount
    );

VOID
IopTrimPageCacheVirtual (
    BOOL TimidEffort
    );

BOOL
IopIsIoBufferPageCacheBackedHelper (
    PFILE_OBJECT FileObject,
    PIO_BUFFER IoBuffer,
    ULONGLONG Offset,
    UINTN SizeInBytes
    );

KSTATUS
IopUnmapPageCacheEntrySections (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    PBOOL PageWasDirty
    );

KSTATUS
IopRemovePageCacheEntryVirtualAddress (
    PPAGE_CACHE_ENTRY Entry,
    PVOID *VirtualAddress
    );

VOID
IopRemovePageCacheEntryFromTree (
    PPAGE_CACHE_ENTRY PageCacheEntry
    );

VOID
IopUpdatePageCacheEntryList (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    BOOL Created
    );

BOOL
IopIsPageCacheTooBig (
    PUINTN FreePhysicalPages
    );

BOOL
IopIsPageCacheTooMapped (
    PUINTN FreeVirtualPages
    );

VOID
IopCheckFileObjectPageCache (
    PFILE_OBJECT FileObject
    );

//
// -------------------------------------------------------------------- Globals
//

//
// Stores the list head for the page cache entries that are ordered from least
// to most recently used. This will mostly contain clean entries, but could
// have a few dirty entries on it.
//

LIST_ENTRY IoPageCacheCleanList;

//
// Stores the list head for page cache entries that are clean but not mapped.
// The unmap loop moves entries from the clean list to here to avoid iterating
// over them too many times. These entries are considered even less used than
// the clean list.
//

LIST_ENTRY IoPageCacheCleanUnmappedList;

//
// Stores the list head for the list of page cache entries that are ready to be
// removed from the cache. These could be evicted entries or entries that
// belong to a deleted file. A deleted file's entries are not necessarily
// marked evicted, thus the evicted count is not always equal to the size of
// this list.
//

LIST_ENTRY IoPageCacheRemovalList;

//
// Stores a lock to protect access to the lists of page cache entries.
//

PQUEUED_LOCK IoPageCacheListLock;

//
// The count tracks the current number of entries in the cache's tree. This is
// protected by the tree lock.
//

UINTN IoPageCacheEntryCount = 0;

//
// Store the target number of free pages in the system the page cache shoots
// for once low-memory eviction of page cache entries kicks in.
//

UINTN IoPageCacheHeadroomPagesRetreat = 0;

//
// Store the number of free physical pages in the system at (or below) which
// the page cache will start evicting entries.
//

UINTN IoPageCacheHeadroomPagesTrigger = 0;

//
// Store the size of the page cache (in pages) below which the page cache will
// ask for pages to be paged out in an effort to keep the working set in memory.
//

UINTN IoPageCacheMinimumPagesTarget = 0;

//
// Store the minimum size (in pages) below which the page cache will not
// attempt to shrink.
//

UINTN IoPageCacheMinimumPages = 0;

//
// Store the minimum number of pages that must be clean in a low memory
// scenario before the page cache worker stops flushing entries in favor or
// removing clean entries.
//

UINTN IoPageCacheLowMemoryCleanPageMinimum = 0;

//
// The physical page count tracks the current number of physical pages in use
// by the cache. This includes pages that are active in the tree and pages that
// are not in the tree, awaiting destruction.
//

volatile UINTN IoPageCachePhysicalPageCount = 0;

//
// Stores the number of pages in the cache that are dirty.
//

volatile UINTN IoPageCacheDirtyPageCount = 0;

//
// Stores the number of page cache pages that are currently mapped.
//

volatile UINTN IoPageCacheMappedPageCount = 0;

//
// Stores the number of dirty page cache entries that are currently mapped.
//

volatile UINTN IoPageCacheMappedDirtyPageCount = 0;

//
// Store the target number of free virtual pages in the system the page cache
// shoots for once low-memory unmapping of page cache entries kicks in.
//

UINTN IoPageCacheHeadroomVirtualPagesRetreat = 0;

//
// Store the number of free virtual pages in the system at (or below) which
// the page cache will start unmapping entries.
//

UINTN IoPageCacheHeadroomVirtualPagesTrigger = 0;

//
// Store the page cache timer interval.
//

ULONGLONG IoPageCacheCleanInterval;

//
// Store the timer used to trigger the page cache worker.
//

PKTIMER IoPageCacheWorkTimer;

//
// The page cache state records the current state of the cleaning process.
// This is of type PAGE_CACHE_STATE.
//

volatile ULONG IoPageCacheState = PageCacheStateClean;

//
// This stores the last time the page cache was cleaned.
//

INT64_SYNC IoLastPageCacheCleanTime;

//
// Store a bitfield of enabled page cache debug flags. See PAGE_CACHE_DEBUG_*
// for definitions.
//

ULONG IoPageCacheDebugFlags = 0x0;

//
// Store the global page cache entry block allocator.
//

PBLOCK_ALLOCATOR IoPageCacheBlockAllocator;

//
// Store a pointer to the page cache thread itself.
//

PKTHREAD IoPageCacheThread;

//
// Stores a boolean that can be used to disable page cache entries from storing
// virtual addresses.
//

BOOL IoPageCacheDisableVirtualAddresses;

//
// ------------------------------------------------------------------ Functions
//

KERNEL_API
ULONG
IoGetCacheEntryDataSize (
    VOID
    )

/*++

Routine Description:

    This routine returns the size of data stored in each cache entry.

Arguments:

    None.

Return Value:

    Returns the size of the data stored in each cache entry.

--*/

{

    return MmPageSize();
}

KSTATUS
IoGetCacheStatistics (
    PIO_CACHE_STATISTICS Statistics
    )

/*++

Routine Description:

    This routine collects the cache statistics and returns them to the caller.

Arguments:

    Statistics - Supplies a pointer that receives the cache statistics. The
        caller should zero this buffer beforehand and set the version member to
        IO_CACHE_STATISTICS_VERSION. Failure to zero the structure beforehand
        may result in uninitialized data when a driver built for a newer OS is
        run on an older OS.

Return Value:

    Status code.

--*/

{

    ULONGLONG LastCleanTime;

    if (Statistics->Version < IO_CACHE_STATISTICS_VERSION) {
        return STATUS_INVALID_PARAMETER;
    }

    READ_INT64_SYNC(&IoLastPageCacheCleanTime, &LastCleanTime);
    Statistics->EntryCount = IoPageCacheEntryCount;
    Statistics->HeadroomPagesTrigger = IoPageCacheHeadroomPagesTrigger;
    Statistics->HeadroomPagesRetreat = IoPageCacheHeadroomPagesRetreat;
    Statistics->MinimumPagesTarget = IoPageCacheMinimumPagesTarget;
    Statistics->PhysicalPageCount = IoPageCachePhysicalPageCount;
    Statistics->DirtyPageCount = IoPageCacheDirtyPageCount;
    Statistics->LastCleanTime = LastCleanTime;
    return STATUS_SUCCESS;
}

VOID
IoPageCacheEntryAddReference (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine increments the reference count on the given page cache entry.
    It is assumed that callers of this routine either hold the page cache lock
    or already hold a reference on the given page cache entry.

Arguments:

    PageCacheEntry - Supplies a pointer to the page cache entry whose reference
        count should be incremented.

Return Value:

    None.

--*/

{

    ULONG OldReferenceCount;

    OldReferenceCount = RtlAtomicAdd32(&(PageCacheEntry->ReferenceCount), 1);

    ASSERT(OldReferenceCount < 0x1000);

    return;
}

VOID
IoPageCacheEntryReleaseReference (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine decrements the reference count on the given page cache entry.

Arguments:

    PageCacheEntry - Supplies a pointer to the page cache entry whose reference
        count should be incremented.

Return Value:

    None.

--*/

{

    ULONG OldReferenceCount;

    OldReferenceCount = RtlAtomicAdd32(&(PageCacheEntry->ReferenceCount), -1);

    ASSERT((OldReferenceCount != 0) && (OldReferenceCount < 0x1000));

    //
    // Potentially insert the page cache entry on the LRU list if the reference
    // count just dropped to zero.
    //

    if ((OldReferenceCount == 1) &&
        (PageCacheEntry->ListEntry.Next == NULL) &&
        ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0)) {

        KeAcquireQueuedLock(IoPageCacheListLock);

        //
        // Double check to make sure it's not on a list or dirty now.
        //

        if ((PageCacheEntry->ListEntry.Next == NULL) &&
            ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0)) {

            INSERT_BEFORE(&(PageCacheEntry->ListEntry), &IoPageCacheCleanList);
        }

        KeReleaseQueuedLock(IoPageCacheListLock);
    }

    return;
}

PHYSICAL_ADDRESS
IoGetPageCacheEntryPhysicalAddress (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine returns the physical address of the page cache entry.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

Return Value:

    Returns the physical address of the given page cache entry.

--*/

{

    return PageCacheEntry->PhysicalAddress;
}

PVOID
IoGetPageCacheEntryVirtualAddress (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine gets the given page cache entry's virtual address.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

Return Value:

    Returns the virtual address of the given page cache entry.

--*/

{

    PPAGE_CACHE_ENTRY BackingEntry;
    PVOID VirtualAddress;

    //
    // If this page cache entry's virtual address is NULL, but it has a mapped
    // backing entry, then synchronize the two.
    //

    VirtualAddress = PageCacheEntry->VirtualAddress;
    BackingEntry = PageCacheEntry->BackingEntry;

    ASSERT((VirtualAddress == NULL) ||
           (BackingEntry == NULL) ||
           (VirtualAddress == BackingEntry->VirtualAddress));

    if ((VirtualAddress == NULL) && (BackingEntry != NULL)) {

        ASSERT((PageCacheEntry->Flags &
                (PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER |
                 PAGE_CACHE_ENTRY_FLAG_MAPPED)) == 0);

        ASSERT((BackingEntry->Flags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0);

        //
        // Updating the virtual address in the non-backing entry does not need
        // to be atomic because any race would be to set it to the same value.
        // As only backing entries can be set. It also does not set the mapped
        // flag because the backing entry actually owns the page.
        //

        VirtualAddress = BackingEntry->VirtualAddress;
        PageCacheEntry->VirtualAddress = VirtualAddress;
    }

    return VirtualAddress;
}

BOOL
IoSetPageCacheEntryVirtualAddress (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    PVOID VirtualAddress
    )

/*++

Routine Description:

    This routine attempts to set the virtual address in the given page cache
    entry. It is assumed that the page cache entry's physical address is mapped
    at the given virtual address.

Arguments:

    PageCacheEntry - Supplies as pointer to the page cache entry.

    VirtualAddress - Supplies the virtual address to set in the page cache
        entry.

Return Value:

    Returns TRUE if the set succeeds or FALSE if another virtual address is
    already set for the page cache entry.

--*/

{

    ULONG OldFlags;
    BOOL Set;
    PPAGE_CACHE_ENTRY UnmappedEntry;

    ASSERT((VirtualAddress != NULL) &&
           (IS_POINTER_ALIGNED(VirtualAddress, MmPageSize()) != FALSE));

    if ((PageCacheEntry->VirtualAddress != NULL) ||
        (IoPageCacheDisableVirtualAddresses != FALSE)) {

        return FALSE;
    }

    UnmappedEntry = PageCacheEntry;
    if (UnmappedEntry->BackingEntry != NULL) {
        UnmappedEntry = UnmappedEntry->BackingEntry;
    }

    Set = FALSE;
    OldFlags = RtlAtomicOr32(&(UnmappedEntry->Flags),
                             PAGE_CACHE_ENTRY_FLAG_MAPPED);

    ASSERT((OldFlags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0);

    if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) == 0) {
        Set = TRUE;
        UnmappedEntry->VirtualAddress = VirtualAddress;
        RtlAtomicAdd(&IoPageCacheMappedPageCount, 1);
        if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
            RtlAtomicAdd(&IoPageCacheMappedDirtyPageCount, 1);
        }
    }

    //
    // Set the original page cache entry too if it's not the one that just took
    // the VA.
    //

    if (UnmappedEntry != PageCacheEntry) {
        VirtualAddress = UnmappedEntry->VirtualAddress;
        if (VirtualAddress != NULL) {

            //
            // Everyone racing should be trying to set the same value.
            //

            ASSERT(((PageCacheEntry->Flags &
                     PAGE_CACHE_ENTRY_FLAG_MAPPED) == 0) &&
                   ((PageCacheEntry->VirtualAddress == NULL) ||
                    (PageCacheEntry->VirtualAddress == VirtualAddress)));

            PageCacheEntry->VirtualAddress = VirtualAddress;
        }
    }

    return Set;
}

BOOL
IoMarkPageCacheEntryDirty (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine marks the given page cache entry as dirty.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

Return Value:

    Returns TRUE if it marked the entry dirty or FALSE if the entry was already
    dirty.

--*/

{

    PPAGE_CACHE_ENTRY DirtyEntry;
    PFILE_OBJECT FileObject;
    BOOL MarkedDirty;

    //
    // Try to get the backing entry if possible.
    //

    if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) == 0) {
        DirtyEntry = PageCacheEntry->BackingEntry;

    } else {

        ASSERT((PageCacheEntry->Flags &
                PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0);

        DirtyEntry = PageCacheEntry;
    }

    //
    // Quick exit if the page cache entry is already dirty.
    //

    if ((DirtyEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
        return FALSE;
    }

    FileObject = DirtyEntry->FileObject;
    KeAcquireSharedExclusiveLockExclusive(FileObject->Lock);

    //
    // Double check the backing entry to make sure the right lock was acquired.
    //

    if (DirtyEntry->BackingEntry != NULL) {

        ASSERT(DirtyEntry == PageCacheEntry);

        KeReleaseSharedExclusiveLockExclusive(FileObject->Lock);
        DirtyEntry = PageCacheEntry->BackingEntry;
        FileObject = DirtyEntry->FileObject;
        KeAcquireSharedExclusiveLockExclusive(FileObject->Lock);
    }

    MarkedDirty = IopMarkPageCacheEntryDirty(DirtyEntry);
    KeReleaseSharedExclusiveLockExclusive(FileObject->Lock);
    return MarkedDirty;
}

KSTATUS
IopInitializePageCache (
    VOID
    )

/*++

Routine Description:

    This routine initializes the page cache.

Arguments:

    None.

Return Value:

    Status code.

--*/

{

    PBLOCK_ALLOCATOR BlockAllocator;
    ULONGLONG CurrentTime;
    ULONG PageShift;
    UINTN PhysicalPages;
    KSTATUS Status;
    UINTN TotalPhysicalPages;
    UINTN TotalVirtualMemory;

    INITIALIZE_LIST_HEAD(&IoPageCacheCleanList);
    INITIALIZE_LIST_HEAD(&IoPageCacheCleanUnmappedList);
    INITIALIZE_LIST_HEAD(&IoPageCacheRemovalList);
    IoPageCacheListLock = KeCreateQueuedLock();
    if (IoPageCacheListLock == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto InitializePageCacheEnd;
    }

    //
    // Create a timer to schedule the page cache worker.
    //

    IoPageCacheWorkTimer = KeCreateTimer(PAGE_CACHE_ALLOCATION_TAG);
    if (IoPageCacheWorkTimer == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto InitializePageCacheEnd;
    }

    //
    // Create the block allocator for the page cache entry structures.
    //

    BlockAllocator = MmCreateBlockAllocator(
                                    sizeof(PAGE_CACHE_ENTRY),
                                    0,
                                    PAGE_CACHE_BLOCK_ALLOCATOR_EXPANSION_COUNT,
                                    BLOCK_ALLOCATOR_FLAG_TRIM,
                                    PAGE_CACHE_ALLOCATION_TAG);

    if (BlockAllocator == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto InitializePageCacheEnd;
    }

    IoPageCacheBlockAllocator = BlockAllocator;

    //
    // Determine an appropriate limit on the size of the page cache based on
    // the total number of physical pages.
    //

    TotalPhysicalPages = MmGetTotalPhysicalPages();
    PhysicalPages = (TotalPhysicalPages *
                     PAGE_CACHE_MEMORY_HEADROOM_PERCENT_RETREAT) /
                    100;

    if (PhysicalPages > MAX_UINTN) {
        PhysicalPages = MAX_UINTN;
    }

    IoPageCacheHeadroomPagesRetreat = PhysicalPages;

    ASSERT(IoPageCacheHeadroomPagesRetreat > 0);

    PhysicalPages = (TotalPhysicalPages *
                     PAGE_CACHE_MEMORY_HEADROOM_PERCENT_TRIGGER) /
                    100;

    if (PhysicalPages > MAX_UINTN) {
        PhysicalPages = MAX_UINTN;
    }

    IoPageCacheHeadroomPagesTrigger = PhysicalPages;

    ASSERT(IoPageCacheHeadroomPagesTrigger > 0);

    PhysicalPages = (TotalPhysicalPages *
                     PAGE_CACHE_MINIMUM_MEMORY_TARGET_PERCENT) /
                    100;

    if (PhysicalPages > MAX_UINTN) {
        PhysicalPages = MAX_UINTN;
    }

    IoPageCacheMinimumPagesTarget = PhysicalPages;
    PhysicalPages = (TotalPhysicalPages * PAGE_CACHE_MINIMUM_MEMORY_PERCENT) /
                    100;

    if (PhysicalPages > MAX_UINTN) {
        PhysicalPages = MAX_UINTN;
    }

    IoPageCacheMinimumPages = PhysicalPages;
    PhysicalPages = (TotalPhysicalPages *
                     PAGE_CACHE_LOW_MEMORY_CLEAN_PAGE_MINIMUM_PERCENTAGE) /
                    100;

    if (PhysicalPages > MAX_UINTN) {
        PhysicalPages = MAX_UINTN;
    }

    IoPageCacheLowMemoryCleanPageMinimum = PhysicalPages;

    ASSERT(IoPageCacheLowMemoryCleanPageMinimum > 0);

    if (IoPageCacheLowMemoryCleanPageMinimum >
        PAGE_CACHE_LOW_MEMORY_CLEAN_PAGE_MAXIMUM) {

        IoPageCacheLowMemoryCleanPageMinimum =
                                      PAGE_CACHE_LOW_MEMORY_CLEAN_PAGE_MAXIMUM;
    }

    //
    // Determine an appropriate limit on the amount of virtual memory the page
    // cache is allowed to consume based on the total amount of system virtual
    // memory.
    //

    PageShift = MmPageShift();
    TotalVirtualMemory = MmGetTotalVirtualMemory();
    if (TotalVirtualMemory < MAX_ULONG) {
        IoPageCacheHeadroomVirtualPagesTrigger =
                  PAGE_CACHE_SMALL_VIRTUAL_HEADROOM_TRIGGER_BYTES >> PageShift;

        IoPageCacheHeadroomVirtualPagesRetreat =
                  PAGE_CACHE_SMALL_VIRTUAL_HEADROOM_RETREAT_BYTES >> PageShift;

    } else {
        IoPageCacheHeadroomVirtualPagesTrigger =
                  PAGE_CACHE_LARGE_VIRTUAL_HEADROOM_TRIGGER_BYTES >> PageShift;

        IoPageCacheHeadroomVirtualPagesRetreat =
                  PAGE_CACHE_LARGE_VIRTUAL_HEADROOM_RETREAT_BYTES >> PageShift;
    }

    IoPageCacheCleanInterval =
                  KeConvertMicrosecondsToTimeTicks(PAGE_CACHE_CLEAN_DELAY_MIN);

    CurrentTime = HlQueryTimeCounter();
    WRITE_INT64_SYNC(&IoLastPageCacheCleanTime, CurrentTime);

    //
    // With success on the horizon, create a thread to handle the background
    // page cache entry removal and flushing work.
    //

    Status = PsCreateKernelThread(IopPageCacheThread,
                                  NULL,
                                  "IopPageCacheThread");

    if (!KSUCCESS(Status)) {
        goto InitializePageCacheEnd;
    }

    Status = STATUS_SUCCESS;

InitializePageCacheEnd:
    if (!KSUCCESS(Status)) {
        if (IoPageCacheListLock != NULL) {
            KeDestroyQueuedLock(IoPageCacheListLock);
            IoPageCacheListLock = NULL;
        }

        if (IoPageCacheWorkTimer != NULL) {
            KeDestroyTimer(IoPageCacheWorkTimer);
            IoPageCacheWorkTimer = NULL;
        }

        if (IoPageCacheBlockAllocator != NULL) {
            MmDestroyBlockAllocator(IoPageCacheBlockAllocator);
            IoPageCacheBlockAllocator = NULL;
        }
    }

    return Status;
}

PPAGE_CACHE_ENTRY
IopLookupPageCacheEntry (
    PFILE_OBJECT FileObject,
    ULONGLONG Offset
    )

/*++

Routine Description:

    This routine searches for a page cache entry based on the file object and
    offset. If found, this routine takes a reference on the page cache entry.

Arguments:

    FileObject - Supplies a pointer to a file object for the device or file.

    Offset - Supplies an offset into the file or device.

Return Value:

    Returns a pointer to the found page cache entry on success, or NULL on
    failure.

--*/

{

    PPAGE_CACHE_ENTRY FoundEntry;

    ASSERT(KeIsSharedExclusiveLockHeld(FileObject->Lock));

    FoundEntry = IopLookupPageCacheEntryHelper(FileObject, Offset);

    //
    // If the entry was found for a write operation, then update its list
    // information.
    //

    if (FoundEntry != NULL) {
        IopUpdatePageCacheEntryList(FoundEntry, FALSE);
    }

    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_LOOKUP) != 0) {
        if (FoundEntry != NULL) {
            RtlDebugPrint("PAGE CACHE: Lookup for file object (0x%08x) at "
                          "offset 0x%I64x succeeded: cache entry 0x%08x, "
                          "physical address 0x%I64x, reference count %d, "
                          "flags 0x%08x.\n",
                          FileObject,
                          Offset,
                          FoundEntry,
                          FoundEntry->PhysicalAddress,
                          FoundEntry->ReferenceCount,
                          FoundEntry->Flags);

        } else {
            RtlDebugPrint("PAGE CACHE: Lookup for file object (0x08%x) at "
                          "offset 0x%I64x failed.\n",
                          FileObject,
                          Offset);
        }
    }

    return FoundEntry;
}

PPAGE_CACHE_ENTRY
IopCreateOrLookupPageCacheEntry (
    PFILE_OBJECT FileObject,
    PVOID VirtualAddress,
    PHYSICAL_ADDRESS PhysicalAddress,
    ULONGLONG Offset,
    PPAGE_CACHE_ENTRY LinkEntry,
    PBOOL EntryCreated
    )

/*++

Routine Description:

    This routine creates a page cache entry and inserts it into the cache. Or,
    if a page cache entry already exists for the supplied file object and
    offset, it returns the existing entry. The file object lock must be held
    exclusive already.

Arguments:

    FileObject - Supplies a pointer to a file object for the device or file
        that owns the contents of the physical page.

    VirtualAddress - Supplies an optional virtual address of the page.

    PhysicalAddress - Supplies the physical address of the page.

    Offset - Supplies the offset into the file or device where the page is
        from.

    LinkEntry - Supplies an optional pointer to a page cache entry that is
        to share the physical address with this new page cache entry if it gets
        inserted.

    EntryCreated - Supplies an optional pointer that receives a boolean
        indicating whether or not a new page cache entry was created.

Return Value:

    Returns a pointer to a page cache entry on success, or NULL on failure.

--*/

{

    BOOL Created;
    PPAGE_CACHE_ENTRY ExistingCacheEntry;
    PPAGE_CACHE_ENTRY PageCacheEntry;

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(FileObject->Lock));
    ASSERT((LinkEntry == NULL) ||
           (LinkEntry->PhysicalAddress == PhysicalAddress));

    Created = FALSE;

    //
    // Allocate and initialize a new page cache entry.
    //

    PageCacheEntry = IopCreatePageCacheEntry(FileObject,
                                             VirtualAddress,
                                             PhysicalAddress,
                                             Offset);

    if (PageCacheEntry == NULL) {
        goto CreateOrLookupPageCacheEntryEnd;
    }

    //
    // Try to insert the entry. If someone else beat this to the punch, then
    // use the existing cache entry.
    //

    ExistingCacheEntry = IopLookupPageCacheEntryHelper(FileObject, Offset);
    if (ExistingCacheEntry == NULL) {
        IopInsertPageCacheEntry(PageCacheEntry, LinkEntry);
        Created = TRUE;
    }

    //
    // If an existing entry was found, then release the allocated entry.
    //

    if (Created == FALSE) {

        ASSERT(PageCacheEntry->ReferenceCount == 1);

        PageCacheEntry->ReferenceCount = 0;
        IopDestroyPageCacheEntry(PageCacheEntry);
        PageCacheEntry = ExistingCacheEntry;
    }

    //
    // Put the page cache entry on the appropriate list.
    //

    IopUpdatePageCacheEntryList(PageCacheEntry, Created);
    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_INSERTION) != 0) {
        if (Created != FALSE) {
            RtlDebugPrint("PAGE CACHE: Inserted new entry for file object "
                          "(0x%08x) at offset 0x%I64x: cache entry 0x%08x, "
                          "physical address 0x%I64x, reference count %d, "
                          "flags 0x%08x.\n",
                          FileObject,
                          Offset,
                          PageCacheEntry,
                          PageCacheEntry->PhysicalAddress,
                          PageCacheEntry->ReferenceCount,
                          PageCacheEntry->Flags);

        } else {
            RtlDebugPrint("PAGE CACHE: Insert found existing entry for file "
                          "object (0x%08x) at offset 0x%I64x: cache entry "
                          "0x%08x, physical address 0x%I64x, reference count "
                          "%d, flags 0x%08x.\n",
                          FileObject,
                          Offset,
                          PageCacheEntry,
                          PageCacheEntry->PhysicalAddress,
                          PageCacheEntry->ReferenceCount,
                          PageCacheEntry->Flags);
        }
    }

CreateOrLookupPageCacheEntryEnd:
    if (EntryCreated != NULL) {
        *EntryCreated = Created;
    }

    return PageCacheEntry;
}

PPAGE_CACHE_ENTRY
IopCreateAndInsertPageCacheEntry (
    PFILE_OBJECT FileObject,
    PVOID VirtualAddress,
    PHYSICAL_ADDRESS PhysicalAddress,
    ULONGLONG Offset,
    PPAGE_CACHE_ENTRY LinkEntry
    )

/*++

Routine Description:

    This routine creates a page cache entry and inserts it into the cache. The
    caller should be certain that there is not another entry in the cache for
    the same file object and offset and that nothing else is in contention to
    create the same entry.

Arguments:

    FileObject - Supplies a pointer to a file object for the device or file
        that owns the contents of the physical page.

    VirtualAddress - Supplies an optional virtual address of the page.

    PhysicalAddress - Supplies the physical address of the page.

    Offset - Supplies the offset into the file or device where the page is
        from.

    LinkEntry - Supplies an optional pointer to a page cache entry that is to
        share the physical address with the new page cache entry.

Return Value:

    Returns a pointer to a page cache entry on success, or NULL on failure.

--*/

{

    PPAGE_CACHE_ENTRY PageCacheEntry;

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(FileObject->Lock) != FALSE);
    ASSERT((LinkEntry == NULL) ||
           (LinkEntry->PhysicalAddress == PhysicalAddress));

    //
    // Allocate and initialize a new page cache entry.
    //

    PageCacheEntry = IopCreatePageCacheEntry(FileObject,
                                             VirtualAddress,
                                             PhysicalAddress,
                                             Offset);

    if (PageCacheEntry == NULL) {
        goto CreateAndInsertPageCacheEntryEnd;
    }

    //
    // Insert the entry. Nothing should beat this to the punch.
    //

    ASSERT(IopLookupPageCacheEntryHelper(FileObject, Offset) == NULL);

    IopInsertPageCacheEntry(PageCacheEntry, LinkEntry);

    //
    // Add the newly created page cach entry to the appropriate list.
    //

    IopUpdatePageCacheEntryList(PageCacheEntry, TRUE);
    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_INSERTION) != 0) {
        RtlDebugPrint("PAGE CACHE: Inserted new entry for file object "
                      "(0x%08x) at offset 0x%I64x: cache entry 0x%08x, "
                      "physical address 0x%I64x, reference count %d, "
                      "flags 0x%08x.\n",
                      FileObject,
                      Offset,
                      PageCacheEntry,
                      PageCacheEntry->PhysicalAddress,
                      PageCacheEntry->ReferenceCount,
                      PageCacheEntry->Flags);
    }

CreateAndInsertPageCacheEntryEnd:
    return PageCacheEntry;
}

KSTATUS
IopCopyAndCacheIoBuffer (
    PFILE_OBJECT FileObject,
    ULONGLONG FileOffset,
    PIO_BUFFER Destination,
    UINTN CopySize,
    PIO_BUFFER Source,
    UINTN SourceSize,
    UINTN SourceCopyOffset,
    PUINTN BytesCopied
    )

/*++

Routine Description:

    This routine iterates over the source buffer, caching each page and copying
    the pages to the destination buffer starting at the given copy offsets and
    up to the given copy size. The file object lock must be held exclusive
    already.

Arguments:

    FileObject - Supplies a pointer to the file object for the device or file
        that owns the data.

    FileOffset - Supplies an offset into the file that corresponds to the
        beginning of the source I/O buffer.

    Destination - Supplies a pointer to the destination I/O buffer.

    CopySize - Supplies the maximum number of bytes that can be copied
        to the destination.

    Source - Supplies a pointer to the source I/O buffer.

    SourceSize - Supplies the number of bytes in the source that should be
        cached.

    SourceCopyOffset - Supplies the offset into the source buffer where the
        copy to the destination should start.

    BytesCopied - Supplies a pointer that receives the number of bytes copied
        to the destination buffer.

Return Value:

    Status code.

--*/

{

    BOOL Created;
    PIO_BUFFER_FRAGMENT Fragment;
    UINTN FragmentIndex;
    UINTN FragmentOffset;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    ULONG PageSize;
    PHYSICAL_ADDRESS PhysicalAddress;
    PPAGE_CACHE_ENTRY SourceEntry;
    UINTN SourceOffset;
    KSTATUS Status;
    PVOID VirtualAddress;

    *BytesCopied = 0;
    PageSize = MmPageSize();

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(FileObject->Lock) != FALSE);
    ASSERT(IS_ALIGNED(SourceSize, PageSize) != FALSE);
    ASSERT(IS_ALIGNED(CopySize, PageSize) != FALSE);

    Fragment = Source->Fragment;
    FragmentIndex = 0;
    FragmentOffset = 0;
    SourceOffset = 0;
    while (SourceSize != 0) {

        ASSERT(FragmentIndex < Source->FragmentCount);
        ASSERT(IS_ALIGNED(Fragment->Size, PageSize) != FALSE);

        //
        // If the source buffer is already backed by a page cache entry at the
        // current offset, then this new page cache entry should try to
        // reference that entry. Otherwise, it will directly own the physical
        // page.
        //

        SourceEntry = MmGetIoBufferPageCacheEntry(Source, SourceOffset);
        PhysicalAddress = Fragment->PhysicalAddress + FragmentOffset;

        ASSERT((SourceEntry == NULL) ||
               (SourceEntry->PhysicalAddress == PhysicalAddress));

        //
        // Find a virtual address for the page cache entry that is about to be
        // created. Prefer the address in the source's page cache entry, but
        // also use the source I/O buffer's virtual address if present.
        //

        VirtualAddress = NULL;
        if (SourceEntry != NULL) {
            VirtualAddress = SourceEntry->VirtualAddress;
        }

        if ((VirtualAddress == NULL) && (Fragment->VirtualAddress != NULL)) {
            VirtualAddress = Fragment->VirtualAddress + FragmentOffset;

            //
            // If there is a source page cache entry and it had no VA, it is
            // current mapped at the determined VA. Transfer ownership to the
            // page cache entry.
            //

            if (SourceEntry != NULL) {
                IoSetPageCacheEntryVirtualAddress(SourceEntry, VirtualAddress);
            }
        }

        //
        // Try to create a page cache entry for this fragment of the source.
        //

        PageCacheEntry = IopCreateOrLookupPageCacheEntry(FileObject,
                                                         VirtualAddress,
                                                         PhysicalAddress,
                                                         FileOffset,
                                                         SourceEntry,
                                                         &Created);

        if (PageCacheEntry == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto CopyAndCacheIoBufferEnd;
        }

        //
        // If a cache entry was created for this physical page and the source
        // was not already backed by the page cache, then the source buffer
        // needs to take a reference on it. Otherwise the source buffer will
        // incorrectly free this physical page. Initialize the source buffer at
        // this offset with the created page cache entry.
        //

        if ((Created != FALSE) && (SourceEntry == NULL)) {
            MmSetIoBufferPageCacheEntry(Source, SourceOffset, PageCacheEntry);
        }

        //
        // If the source offset equals the copy offset, and there is more
        // to "copy", initialize the destination buffer with this page
        // cache entry.
        //

        if ((SourceOffset == SourceCopyOffset) && (CopySize != 0)) {
            MmIoBufferAppendPage(Destination,
                                 PageCacheEntry,
                                 NULL,
                                 INVALID_PHYSICAL_ADDRESS);

            SourceCopyOffset += PageSize;
            CopySize -= PageSize;
            *BytesCopied += PageSize;
        }

        //
        // Always release the reference taken by create or lookup. The I/O
        // buffer initialization routines took the necessary references.
        //

        IoPageCacheEntryReleaseReference(PageCacheEntry);
        FileOffset += PageSize;
        SourceOffset += PageSize;
        SourceSize -= PageSize;
        FragmentOffset += PageSize;

        //
        // If the end of this fragment has been reached, moved to the next.
        //

        if (FragmentOffset == Fragment->Size) {
            Fragment += 1;
            FragmentIndex += 1;
            FragmentOffset = 0;
        }
    }

    Status = STATUS_SUCCESS;

CopyAndCacheIoBufferEnd:
    return Status;
}

KSTATUS
IopFlushPageCacheEntries (
    PFILE_OBJECT FileObject,
    ULONGLONG Offset,
    ULONGLONG Size,
    ULONG Flags,
    PUINTN PageCount
    )

/*++

Routine Description:

    This routine flushes the page cache entries for the given file object
    starting at the given offset for the requested size. This routine does not
    return until all file data has successfully been written to disk. It does
    not guarantee that file meta-data has been flushed to disk.

Arguments:

    FileObject - Supplies a pointer to a file object for the device or file.

    Offset - Supplies the offset from the beginning of the file or device where
        the flush should be done.

    Size - Supplies the size, in bytes, of the region to flush. Supply a value
        of -1 to flush from the given offset to the end of the file.

    Flags - Supplies a bitmask of I/O flags. See IO_FLAG_* for definitions.

    PageCount - Supplies an optional pointer describing how many pages to flush.
        On output this value will be decreased by the number of pages actually
        flushed. Supply NULL to flush all pages in the size range.

Return Value:

    Status code.

--*/

{

    PPAGE_CACHE_ENTRY BackingEntry;
    BOOL BytesFlushed;
    PPAGE_CACHE_ENTRY CacheEntry;
    UINTN CleanStreak;
    PIO_BUFFER FlushBuffer;
    ULONGLONG FlushNextOffset;
    UINTN FlushSize;
    LIST_ENTRY LocalList;
    PRED_BLACK_TREE_NODE Node;
    BOOL PageCacheThread;
    UINTN PagesFlushed;
    ULONG PageShift;
    ULONG PageSize;
    PAGE_CACHE_ENTRY SearchEntry;
    BOOL SkipEntry;
    KSTATUS Status;
    KSTATUS TotalStatus;

    PageCacheThread = FALSE;
    BytesFlushed = FALSE;
    CacheEntry = NULL;
    FlushBuffer = NULL;
    PagesFlushed = 0;
    PageShift = MmPageShift();
    Status = STATUS_SUCCESS;
    TotalStatus = STATUS_SUCCESS;
    INITIALIZE_LIST_HEAD(&LocalList);
    if (KeGetCurrentThread() == IoPageCacheThread) {
        PageCacheThread = TRUE;

        ASSERT(KeIsSharedExclusiveLockHeldShared(FileObject->Lock));
    }

    ASSERT((Size == -1ULL) || ((Offset + Size) > Offset));

    //
    // Optimistically mark the file object clean.
    //

    if ((Offset == 0) && (Size == -1ULL) && (PageCount == NULL)) {
        RtlAtomicAnd32(&(FileObject->Flags), ~FILE_OBJECT_FLAG_DIRTY_DATA);
    }

    if (IO_IS_FILE_OBJECT_CACHEABLE(FileObject) == FALSE) {
        goto FlushPageCacheEntriesEnd;
    }

    //
    // Quickly exit if there is nothing to flush.
    //

    if (LIST_EMPTY(&(FileObject->DirtyPageList)) != FALSE) {
        goto FlushPageCacheEntriesEnd;
    }

    //
    // Allocate a buffer to support the maximum allowed flush size.
    //

    FlushBuffer = MmAllocateUninitializedIoBuffer(PAGE_CACHE_FLUSH_MAX, 0);
    if (FlushBuffer == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto FlushPageCacheEntriesEnd;
    }

    PageSize = MmPageSize();

    ASSERT(KeIsSharedExclusiveLockHeld(FileObject->Lock));

    //
    // Determine which page cache entry the flush should start on.
    //

    SearchEntry.FileObject = FileObject;
    FlushNextOffset = Offset;
    FlushSize = 0;
    CleanStreak = 0;
    SearchEntry.Offset = Offset;
    SearchEntry.Flags = 0;
    Node = NULL;

    //
    // Loop over page cache entries. For flush-all operations, iteration
    // grabs the first entry in the dirty list, then iterates using the
    // tree to maximize contiguous runs. Starting from the list avoids chewing
    // up CPU time scanning through the tree. For explicit flush operations of
    // a specific region, iterate using only the tree.
    //

    if ((Size != -1ULL) || (Offset != 0)) {
        Node = RtlRedBlackTreeSearchClosest(&(FileObject->PageCacheTree),
                                            &(SearchEntry.Node),
                                            TRUE);

    //
    // Move all dirty entries over to a local list to avoid processing them
    // many times over.
    //

    } else {
        KeAcquireQueuedLock(IoPageCacheListLock);
        if (!LIST_EMPTY(&(FileObject->DirtyPageList))) {
            MOVE_LIST(&(FileObject->DirtyPageList), &LocalList);
            INITIALIZE_LIST_HEAD(&(FileObject->DirtyPageList));
        }

        KeReleaseQueuedLock(IoPageCacheListLock);
    }

    while (TRUE) {

        //
        // Traverse along the tree if there's already a node.
        //

        if (Node != NULL) {
            Node = RtlRedBlackTreeGetNextNode(&(FileObject->PageCacheTree),
                                              FALSE,
                                              Node);
        }

        if ((Node == NULL) && (Size == -1ULL) && (Offset == 0)) {
            KeAcquireQueuedLock(IoPageCacheListLock);
            if (!LIST_EMPTY(&LocalList)) {
                CacheEntry = LIST_VALUE(LocalList.Next,
                                        PAGE_CACHE_ENTRY,
                                        ListEntry);

                Node = &(CacheEntry->Node);
            }

            KeReleaseQueuedLock(IoPageCacheListLock);
        }

        //
        // Stop if there's nothing left.
        //

        if (Node == NULL) {
            break;
        }

        CacheEntry = RED_BLACK_TREE_VALUE(Node, PAGE_CACHE_ENTRY, Node);
        if ((Size != -1ULL) && (CacheEntry->Offset >= (Offset + Size))) {
            break;
        }

        //
        // Determine if the current entry can be skipped.
        //

        SkipEntry = FALSE;
        BackingEntry = CacheEntry->BackingEntry;

        ASSERT(CacheEntry->FileObject == FileObject);

        //
        // If the entry is clean, then it can probably be skipped.
        //

        if ((CacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) {
            SkipEntry = TRUE;

            //
            // If this is a synchronized flush and the backing entry is dirty,
            // then write it out.
            //

            if (((Flags & IO_FLAG_DATA_SYNCHRONIZED) != 0) &&
                (BackingEntry != NULL) &&
                ((BackingEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0)) {

                SkipEntry = FALSE;
            }

            //
            // A certain number of clean pages will be tolerated to batch up
            // writes.
            //

            if ((FlushSize != 0) &&
                (CacheEntry->Offset == FlushNextOffset) &&
                (CleanStreak < PAGE_CACHE_FLUSH_MAX_CLEAN_STREAK)) {

                CleanStreak += 1;
                SkipEntry = FALSE;
            }

            //
            // If a reference is taken on the clean page then it could become
            // dirty at any time, but non-referenced clean pages should be on
            // a list.
            // TODO: This is not a valid assert, as release reference could be
            // in progress. Remove this if it fires and release reference seems
            // to be in progress on another core.
            //

            ASSERT((CacheEntry->ReferenceCount != 0) ||
                   (CacheEntry->ListEntry.Next != NULL));

        //
        // If the entry is not within the bounds of the provided offset and
        // size then it can be skipped.
        //

        } else {
            if ((CacheEntry->Offset + PageSize) <= Offset) {
                SkipEntry = TRUE;

            } else if ((Size != -1ULL) &&
                       (CacheEntry->Offset >= (Offset + Size))) {

                SkipEntry = TRUE;
            }

            //
            // If it's dirty and it counts, then reset any clean streaks.
            //

            if (SkipEntry == FALSE) {
                CleanStreak = 0;
            }
        }

        //
        // Potentially move to the next set of entries.
        //

        if (SkipEntry != FALSE) {
            if ((Size == -1ULL) && (Offset == 0)) {
                Node = NULL;
            }

            continue;
        }

        PagesFlushed += 1;

        //
        // Add the cache entry to the flush buffer if necessary,
        // potentially looping again to try to add more pages.
        //

        if ((FlushSize == 0) || (CacheEntry->Offset == FlushNextOffset)) {
            MmIoBufferAppendPage(FlushBuffer,
                                 CacheEntry,
                                 NULL,
                                 INVALID_PHYSICAL_ADDRESS);

            FlushSize += PageSize;
            FlushNextOffset = CacheEntry->Offset + PageSize;
            if (FlushSize < PAGE_CACHE_FLUSH_MAX) {
                continue;
            }

            //
            // Clear out the cache entry to indicate it's been handled.
            //

            CacheEntry = NULL;
        }

        ASSERT(FlushSize != 0);

        //
        // No need to flush any trailing clean entries on the end.
        //

        ASSERT(FlushSize > (CleanStreak << PageShift));

        FlushSize -= CleanStreak << PageShift;
        Status = IopFlushPageCacheBuffer(FlushBuffer, FlushSize, Flags);
        if (!KSUCCESS(Status)) {
            TotalStatus = Status;

        } else {
            BytesFlushed = TRUE;
        }

        //
        // Prepare the flush buffer to be used again.
        //

        MmResetIoBuffer(FlushBuffer);
        FlushSize = 0;
        CleanStreak = 0;

        //
        // Stop if enough pages were flushed.
        //

        if ((PageCount != NULL) && (PagesFlushed >= *PageCount)) {
            break;
        }

        //
        // If this cache entry has not been dealt with, add it to the buffer
        // now.
        //

        if (CacheEntry != NULL) {
            MmIoBufferAppendPage(FlushBuffer,
                                 CacheEntry,
                                 NULL,
                                 INVALID_PHYSICAL_ADDRESS);

            FlushSize = PageSize;
            FlushNextOffset = CacheEntry->Offset + PageSize;

        //
        // Reset the iteration for a completely blank slate.
        //

        } else {
            if ((Size == -1ULL) && (Offset == 0)) {
                Node = NULL;
            }
        }

        //
        // If this is an attempt to flush the entire cache, check on the memory
        // warning level, it may be necessary to stop the flush and evict some
        // entries. Only do this if the minimum number of pages have been
        // cleaned.
        //

        if (PageCacheThread != FALSE) {
            if ((IopIsPageCacheTooBig(NULL) != FALSE) &&
                ((IoPageCachePhysicalPageCount -
                  IoPageCacheDirtyPageCount) >
                 IoPageCacheLowMemoryCleanPageMinimum)) {

                Status = STATUS_TRY_AGAIN;
                goto FlushPageCacheEntriesEnd;
            }

            //
            // If others are trying to get in, be polite.
            //

            if (KeIsSharedExclusiveLockContended(FileObject->Lock) != FALSE) {
                KeReleaseSharedExclusiveLockShared(FileObject->Lock);
                KeAcquireSharedExclusiveLockShared(FileObject->Lock);
            }
        }
    }

    //
    // If the loop completed and there was something left to flush, do it now.
    //

    ASSERT(FlushSize >= (CleanStreak << PageShift));

    FlushSize -= CleanStreak << PageShift;
    if (FlushSize != 0) {
        Status = IopFlushPageCacheBuffer(FlushBuffer, FlushSize, Flags);
        if (!KSUCCESS(Status)) {
            TotalStatus = Status;

        } else {
            BytesFlushed = TRUE;
        }
    }

    //
    // Validate the dirty lists if the debug flag is set. This is very slow,
    // and should only be turned on if actively debugging missing dirty page
    // cache pages.
    //

    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_DIRTY_LISTS) != 0) {
        IopCheckFileObjectPageCache(FileObject);
    }

    Status = STATUS_SUCCESS;

FlushPageCacheEntriesEnd:

    //
    // If there are still entries on the local list, put those back on the
    // dirty list.
    //

    if (!LIST_EMPTY(&LocalList)) {
        KeAcquireQueuedLock(IoPageCacheListLock);
        APPEND_LIST(&LocalList, &(FileObject->DirtyPageList));
        KeReleaseQueuedLock(IoPageCacheListLock);
    }

    if ((!KSUCCESS(Status)) && (KSUCCESS(TotalStatus))) {
        TotalStatus = Status;
    }

    //
    // If writing to a disk and the synchronized flag is not set, then
    // a sync operation will need to be performed on this disk.
    //

    if ((BytesFlushed != FALSE) &&
        (FileObject->Properties.Type == IoObjectBlockDevice) &&
        ((Flags & IO_FLAG_DATA_SYNCHRONIZED) == 0)) {

        Status = IopSynchronizeBlockDevice(FileObject->Device);
        if (!KSUCCESS(Status)) {
            TotalStatus = Status;
        }
    }

    if (FlushBuffer != NULL) {
        MmFreeIoBuffer(FlushBuffer);
    }

    if (PageCount != NULL) {
        if (PagesFlushed > *PageCount) {
            *PageCount = 0;

        } else {
            *PageCount -= PagesFlushed;
        }
    }

    //
    // Mark the file object as dirty if something went wrong.
    //

    if (!KSUCCESS(TotalStatus)) {
        RtlAtomicOr32(&(FileObject->Flags), FILE_OBJECT_FLAG_DIRTY_DATA);
    }

    return TotalStatus;
}

VOID
IopEvictPageCacheEntries (
    PFILE_OBJECT FileObject,
    ULONGLONG Offset,
    ULONG Flags
    )

/*++

Routine Description:

    This routine attempts to evict the page cache entries for a given file or
    device, as specified by the file object. The flags specify how aggressive
    this routine should be. The file object lock must already be held exclusive.

Arguments:

    FileObject - Supplies a pointer to a file object for the device or file.

    Flags - Supplies a bitmask of eviction flags. See
        PAGE_CACHE_EVICTION_FLAG_* for definitions.

    Offset - Supplies the starting offset into the file or device after which
        all page cache entries should be evicted.

Return Value:

    None.

--*/

{

    PPAGE_CACHE_ENTRY CacheEntry;
    LIST_ENTRY DestroyListHead;
    PRED_BLACK_TREE_NODE Node;
    PAGE_CACHE_ENTRY SearchEntry;

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(FileObject->Lock) != FALSE);

    if (IO_IS_FILE_OBJECT_CACHEABLE(FileObject) == FALSE) {
        return;
    }

    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_EVICTION) != 0) {
        RtlDebugPrint("PAGE CACHE: Evicting entries for file object "
                      "(0x%08x): type %d, reference count %d, path count "
                      "%d, offset 0x%I64x.\n",
                      FileObject,
                      FileObject->Properties.Type,
                      FileObject->ReferenceCount,
                      FileObject->PathEntryCount,
                      Offset);
    }

    //
    // The tree is being modified, so the file object lock must be held
    // exclusively.
    //

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(FileObject->Lock));

    //
    // Quickly exit if there is nothing to evict.
    //

    if (RED_BLACK_TREE_EMPTY(&(FileObject->PageCacheTree)) != FALSE) {
        return;
    }

    //
    // Iterate over the file object's tree of page cache entries.
    //

    INITIALIZE_LIST_HEAD(&DestroyListHead);

    //
    // Find the page cache entry in the file object's tree that is closest (but
    // greater than or equal) to the given eviction offset.
    //

    SearchEntry.FileObject = FileObject;
    SearchEntry.Offset = Offset;
    SearchEntry.Flags = 0;
    Node = RtlRedBlackTreeSearchClosest(&(FileObject->PageCacheTree),
                                        &(SearchEntry.Node),
                                        TRUE);

    while (Node != NULL) {
        CacheEntry = LIST_VALUE(Node, PAGE_CACHE_ENTRY, Node);
        Node = RtlRedBlackTreeGetNextNode(&(FileObject->PageCacheTree),
                                          FALSE,
                                          Node);

        //
        // Assert this is a cache entry after the eviction offset.
        //

        ASSERT(CacheEntry->Offset >= Offset);

        //
        // If no flags were provided to indicate more forceful behavior, just
        // do a best effort. If there is a reference on the cache entry, that
        // is not from the flush worker (i.e. the busy flag), then skip it.
        //

        if ((Flags == 0) && (CacheEntry->ReferenceCount != 0)) {
            if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_EVICTION) != 0) {
                RtlDebugPrint("PAGE CACHE: Skip evicting entry 0x%08x: file "
                              "object 0x%08x, offset 0x%I64x, physical "
                              "address 0x%I64x, reference count %d, flags "
                              "0x%08x.\n",
                              CacheEntry,
                              CacheEntry->FileObject,
                              CacheEntry->Offset,
                              CacheEntry->PhysicalAddress,
                              CacheEntry->ReferenceCount,
                              CacheEntry->Flags);
            }

            continue;
        }

        //
        // Clean the page to keep the statistics accurate. It's been evicted
        // and will not be written out. Don't move it to the clean list, as
        // this routine will place it on either the evicted list or the local
        // destroy list.
        //

        IopMarkPageCacheEntryClean(CacheEntry, FALSE);

        //
        // If this is a delete operation, then there should not be any open
        // handles for this file object. Therefore there should be no I/O
        // buffers with references to this file object's page cache entries.
        // Truncate is different, as there may be outstanding handles.
        //

        ASSERT(((Flags & PAGE_CACHE_EVICTION_FLAG_DELETE) == 0) ||
               (CacheEntry->ReferenceCount == 0));

        //
        // Remove the node from the page cache tree. It should not be found on
        // look-up again.
        //

        ASSERT(CacheEntry->Node.Parent != NULL);

        IopRemovePageCacheEntryFromTree(CacheEntry);

        //
        // Remove the cache entry from its current list. If there are no
        // references, it can be destroyed now. Otherwise, stick it on the list
        // to be destroyed later.
        //

        KeAcquireQueuedLock(IoPageCacheListLock);

        ASSERT((CacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0);

        if (CacheEntry->ListEntry.Next != NULL) {
            LIST_REMOVE(&(CacheEntry->ListEntry));
        }

        if (CacheEntry->ReferenceCount == 0) {
            INSERT_BEFORE(&(CacheEntry->ListEntry), &DestroyListHead);

        } else {
            INSERT_BEFORE(&(CacheEntry->ListEntry), &IoPageCacheRemovalList);
        }

        KeReleaseQueuedLock(IoPageCacheListLock);
    }

    //
    // With the evicted page cache entries removed from the cache, loop through
    // and destroy them. This gets called by truncate and device removal, so
    // releasing the last file object reference and generating additional I/O
    // here should be okay (this should not be in a recursive I/O path).
    //

    IopDestroyPageCacheEntries(&DestroyListHead);
    return;
}

BOOL
IopIsIoBufferPageCacheBacked (
    PFILE_OBJECT FileObject,
    PIO_BUFFER IoBuffer,
    ULONGLONG Offset,
    UINTN SizeInBytes
    )

/*++

Routine Description:

    This routine determines whether or not the given I/O buffer with data
    targeting the given file object at the given offset is currently backed by
    the page cache, up to the given size. The caller is expected to synchronize
    with eviction via truncate.

Arguments:

    FileObject - Supplies a pointer to a file object.

    IoBuffer - Supplies a pointer to an I/O buffer.

    Offset - Supplies an offset into the file or device object.

    SizeInBytes - Supplies the number of bytes in the I/O buffer that should be
        cache backed.

Return Value:

    Returns TRUE if the I/O buffer is backed by valid page cache entries, or
    FALSE otherwise.

--*/

{

    BOOL Backed;
    ULONG PageSize;

    ASSERT(IoBuffer->FragmentCount != 0);

    //
    // It is assumed that if the first page of the I/O buffer is backed by the
    // page cache then all pages are backed by the page cache.
    //

    PageSize = MmPageSize();
    Backed = IopIsIoBufferPageCacheBackedHelper(FileObject,
                                                IoBuffer,
                                                Offset,
                                                PageSize);

    //
    // Assert that the assumption above is correct.
    //

    ASSERT((Backed == FALSE) ||
           (IopIsIoBufferPageCacheBackedHelper(FileObject,
                                               IoBuffer,
                                               Offset,
                                               ALIGN_RANGE_UP(SizeInBytes,
                                                              PageSize))));

    return Backed;
}

VOID
IopSchedulePageCacheThread (
    VOID
    )

/*++

Routine Description:

    This routine schedules a cleaning of the page cache for some time in the
    future.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PAGE_CACHE_STATE OldState;
    KSTATUS Status;

    //
    // Do a quick exit check without the atomic first.
    //

    if (IoPageCacheState == PageCacheStateDirty) {
        return;
    }

    //
    // Try to take the state from clean to dirty. If this thread won, then
    // queue the timer.
    //

    OldState = RtlAtomicCompareExchange32(&IoPageCacheState,
                                          PageCacheStateDirty,
                                          PageCacheStateClean);

    if (OldState == PageCacheStateClean) {

        ASSERT(IoPageCacheCleanInterval != 0);

        Status = KeQueueTimer(IoPageCacheWorkTimer,
                              TimerQueueSoftWake,
                              0,
                              IoPageCacheCleanInterval,
                              0,
                              NULL);

        ASSERT(KSUCCESS(Status));
    }

    return;
}

ULONGLONG
IopGetPageCacheEntryOffset (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine gets the file or device offset of the given page cache entry.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

Return Value:

    Returns the file or device offset of the given page cache entry.

--*/

{

    return PageCacheEntry->Offset;
}

BOOL
IopMarkPageCacheEntryClean (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    BOOL MoveToCleanList
    )

/*++

Routine Description:

    This routine marks the given page cache entry as clean.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

    MoveToCleanList - Supplies a boolean indicating if the page cache entry
        should be moved to the list of clean page cache entries.

Return Value:

    Returns TRUE if it marked the entry clean or FALSE if the entry was already
    clean.

--*/

{

    BOOL MarkedClean;
    ULONG OldFlags;

    //
    // Quick exit check before banging around atomically.
    //

    if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) {
        return FALSE;
    }

    //
    // Marking a page cache entry clean requires having a reference on the
    // entry or holding the tree lock.
    //

    ASSERT((PageCacheEntry->ReferenceCount != 0) ||
           (KeIsSharedExclusiveLockHeld(PageCacheEntry->FileObject->Lock)));

    OldFlags = RtlAtomicAnd32(&(PageCacheEntry->Flags),
                              ~PAGE_CACHE_ENTRY_FLAG_DIRTY);

    //
    // Return that this routine marked the page clean based on the old value.
    // Additional decrement the dirty page count if this entry owns the page.
    //

    if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {

        ASSERT((OldFlags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0);

        RtlAtomicAdd(&IoPageCacheDirtyPageCount, (UINTN)-1);
        if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
            RtlAtomicAdd(&IoPageCacheMappedDirtyPageCount, (UINTN)-1);
        }

        MarkedClean = TRUE;

        //
        // Remove the entry from the dirty list.
        //

        KeAcquireQueuedLock(IoPageCacheListLock);

        ASSERT((PageCacheEntry->ListEntry.Next != NULL) &&
               ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0));

        LIST_REMOVE(&(PageCacheEntry->ListEntry));
        PageCacheEntry->ListEntry.Next = NULL;

        //
        // If requested, move the page cache entry to the back of the LRU list;
        // assume that this page has been fairly recently used on account of it
        // having been dirty. If the page is already on a list, then leave it
        // as its current location.
        //

        if (MoveToCleanList != FALSE) {
            INSERT_BEFORE(&(PageCacheEntry->ListEntry), &IoPageCacheCleanList);
        }

        KeReleaseQueuedLock(IoPageCacheListLock);

    } else {
        MarkedClean = FALSE;
    }

    return MarkedClean;
}

BOOL
IopMarkPageCacheEntryDirty (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine marks the given page cache entry as dirty. The file object
    lock must already be held.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

Return Value:

    Returns TRUE if it marked the entry dirty or FALSE if the entry was already
    dirty.

--*/

{

    PPAGE_CACHE_ENTRY DirtyEntry;
    PFILE_OBJECT FileObject;
    BOOL MarkedDirty;
    ULONG OldFlags;

    //
    // If this page cache entry does not own the physical page then directly
    // mark the backing entry dirty. This causes the system to skip the flush
    // at this page cache entry's layer.
    //

    if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) == 0) {

        ASSERT((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0);

        DirtyEntry = PageCacheEntry->BackingEntry;

    } else {
        DirtyEntry = PageCacheEntry;
    }

    //
    // Quick exit check before banging around atomically.
    //

    if ((DirtyEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
        return FALSE;
    }

    FileObject = DirtyEntry->FileObject;
    if (DirtyEntry != PageCacheEntry) {
        KeAcquireSharedExclusiveLockExclusive(FileObject->Lock);
    }

    OldFlags = RtlAtomicOr32(&(DirtyEntry->Flags), PAGE_CACHE_ENTRY_FLAG_DIRTY);

    ASSERT((OldFlags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0);

    if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) {

        ASSERT((DirtyEntry->VirtualAddress == PageCacheEntry->VirtualAddress) ||
               (PageCacheEntry->VirtualAddress == NULL));

        RtlAtomicAdd(&IoPageCacheDirtyPageCount, 1);
        if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
            RtlAtomicAdd(&IoPageCacheMappedDirtyPageCount, 1);
        }

        MarkedDirty = TRUE;

        //
        // Remove the page cache entry from the clean LRU if it's on one.
        //

        KeAcquireQueuedLock(IoPageCacheListLock);
        if (DirtyEntry->ListEntry.Next != NULL) {
            LIST_REMOVE(&(DirtyEntry->ListEntry));
        }

        //
        // Add it to the dirty page list of the file object.
        //

        INSERT_BEFORE(&(DirtyEntry->ListEntry),
                      &(FileObject->DirtyPageList));

        KeReleaseQueuedLock(IoPageCacheListLock);
        IopMarkFileObjectDirty(DirtyEntry->FileObject);

    } else {
        MarkedDirty = FALSE;
    }

    if (DirtyEntry != PageCacheEntry) {
        KeReleaseSharedExclusiveLockExclusive(FileObject->Lock);
    }

    return MarkedDirty;
}

KSTATUS
IopCopyIoBufferToPageCacheEntry (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    ULONG PageOffset,
    PIO_BUFFER SourceBuffer,
    UINTN SourceOffset,
    ULONG ByteCount
    )

/*++

Routine Description:

    This routine copies up to a page from the given source buffer to the given
    page cache entry.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

    PageOffset - Supplies an offset into the page where the copy should begin.

    SourceBuffer - Supplies a pointer to the source buffer where the data
        originates.

    SourceOffset - Supplies an offset into the the source buffer where the data
        copy should begin.

    ByteCount - Supplies the number of bytes to copy.

Return Value:

    Status code.

--*/

{

    IO_BUFFER PageCacheBuffer;
    KSTATUS Status;

    //
    // Initialize the I/O buffer with the page cache entry. This takes an
    // additional reference on the page cache entry.
    //

    Status = MmInitializeIoBuffer(&PageCacheBuffer,
                                  NULL,
                                  INVALID_PHYSICAL_ADDRESS,
                                  0,
                                  IO_BUFFER_FLAG_KERNEL_MODE_DATA);

    if (!KSUCCESS(Status)) {
        goto CopyIoBufferToPageCacheEntryEnd;
    }

    MmIoBufferAppendPage(&PageCacheBuffer,
                         PageCacheEntry,
                         NULL,
                         INVALID_PHYSICAL_ADDRESS);

    //
    // Copy the contents of the source to the page cache entry.
    //

    Status = MmCopyIoBuffer(&PageCacheBuffer,
                            PageOffset,
                            SourceBuffer,
                            SourceOffset,
                            ByteCount);

    if (!KSUCCESS(Status)) {
        goto CopyIoBufferToPageCacheEntryEnd;
    }

    IopMarkPageCacheEntryDirty(PageCacheEntry);

CopyIoBufferToPageCacheEntryEnd:
    MmFreeIoBuffer(&PageCacheBuffer);
    return Status;
}

BOOL
IopCanLinkPageCacheEntry (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    PFILE_OBJECT FileObject
    )

/*++

Routine Description:

    This routine determines if the given page cache entry could link with a
    page cache entry for the given file object.

Arguments:

    PageCacheEntry - Supplies a pointer to a page cache entry.

    FileObject - Supplies a pointer to a file object.

Return Value:

    Returns TRUE if the page cache entry could be linked to a page cache entry
    with the given file object or FALSE otherwise.

--*/

{

    IO_OBJECT_TYPE PageCacheType;

    ASSERT(IO_IS_FILE_OBJECT_CACHEABLE(FileObject) != FALSE);

    PageCacheType = PageCacheEntry->FileObject->Properties.Type;
    if (IS_IO_OBJECT_TYPE_LINKABLE(PageCacheType) == FALSE) {
        return FALSE;
    }

    if (FileObject->Properties.Type == PageCacheType) {
        return FALSE;
    }

    return TRUE;
}

BOOL
IopLinkPageCacheEntries (
    PPAGE_CACHE_ENTRY LowerEntry,
    PPAGE_CACHE_ENTRY UpperEntry
    )

/*++

Routine Description:

    This routine attempts to link the given link entry to the page cache entry
    so that they can begin sharing a physical page that is currently used by
    the link entry.

Arguments:

    LowerEntry - Supplies a pointer to the lower (disk) level page cache entry
        whose physical address is to be modified. The caller should ensure that
        its reference on this entry does not come from an I/O buffer or else
        the physical address in the I/O buffer would be invalid. The file
        object lock for this entry must already be held exclusive.

    UpperEntry - Supplies a pointer to the upper (file) page cache entry
        that currently owns the physical page to be shared.

Return Value:

    Returns TRUE if the two page cache entries are already connected or if the
    routine is successful. It returns FALSE otherwise and both page cache
    entries should continue to use their own physical pages.

--*/

{

    ULONG ClearFlags;
    ULONG Delta;
    IO_OBJECT_TYPE LowerType;
    ULONG OldFlags;
    ULONG PageSize;
    PHYSICAL_ADDRESS PhysicalAddress;
    BOOL Result;
    KSTATUS Status;
    IO_OBJECT_TYPE UpperType;
    PVOID VirtualAddress;

    //
    // The lower file object lock must be held exclusively so that no more
    // references can be taken on the page cache entries.
    //

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(LowerEntry->FileObject->Lock));
    ASSERT(LowerEntry->ReferenceCount > 0);
    ASSERT(UpperEntry->ReferenceCount > 0);

    LowerType = LowerEntry->FileObject->Properties.Type;
    UpperType = UpperEntry->FileObject->Properties.Type;

    //
    // Page cache entries with the same I/O type are not allowed to be linked.
    //

    if (LowerType == UpperType) {
        return FALSE;
    }

    //
    // If the two entries are already linked, do nothing.
    //

    if ((LowerType == IoObjectBlockDevice) &&
        (IO_IS_CACHEABLE_FILE(UpperType))) {

        if (UpperEntry->BackingEntry == LowerEntry) {
            return TRUE;
        }

    } else {

        ASSERT(FALSE);

        return FALSE;
    }

    //
    // If the page cache entry that is to be updated has more than one
    // reference then this cannot proceed.
    //

    if (LowerEntry->ReferenceCount != 1) {
        return FALSE;
    }

    VirtualAddress = NULL;
    PhysicalAddress = INVALID_PHYSICAL_ADDRESS;

    //
    // Both entries should be page owners.
    //

    ASSERT((LowerEntry->Flags & UpperEntry->Flags &
            PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0);

    //
    // Make sure no one has the disk mmaped, since its physical page is about
    // to be destroyed.
    //

    Status = IopUnmapPageCacheEntrySections(LowerEntry, NULL);
    if (!KSUCCESS(Status)) {
        Result = FALSE;
        goto LinkPageCacheEntriesEnd;
    }

    //
    // The upper entry better not be dirty, because the accounting numbers
    // would be off otherwise, and it would result in a dirty non page owner.
    //

    ASSERT((UpperEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0);

    //
    // If the flags differ in mappedness, clear the old mapped flag.
    //

    Delta = LowerEntry->Flags ^ UpperEntry->Flags;
    if ((Delta & LowerEntry->Flags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
        OldFlags = RtlAtomicAnd32(&(LowerEntry->Flags),
                                  ~PAGE_CACHE_ENTRY_FLAG_MAPPED);

        if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
            RtlAtomicAdd(&IoPageCacheMappedPageCount, (UINTN)-1);
            if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
                RtlAtomicAdd(&IoPageCacheMappedDirtyPageCount, (UINTN)-1);
            }
        }
    }

    //
    // Save the address of the physical page that is to be released and update
    // the entries to share the link entry's page.
    //

    PhysicalAddress = LowerEntry->PhysicalAddress;
    VirtualAddress = LowerEntry->VirtualAddress;
    LowerEntry->PhysicalAddress = UpperEntry->PhysicalAddress;
    LowerEntry->VirtualAddress = UpperEntry->VirtualAddress;

    //
    // Clear the mapped flag here because the backing entry owns the mapped
    // page count for this page.
    //

    ClearFlags = PAGE_CACHE_ENTRY_FLAG_MAPPED |
                 PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER;

    OldFlags = RtlAtomicAnd32(&(UpperEntry->Flags), ~ClearFlags);
    if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
        RtlAtomicAdd(&IoPageCacheMappedPageCount, (UINTN)-1);

        //
        // Transfer the mapped flag over to the lower entry.
        //

        if ((Delta & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
            OldFlags = RtlAtomicOr32(&(LowerEntry->Flags),
                                     PAGE_CACHE_ENTRY_FLAG_MAPPED);

            if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) == 0) {
                RtlAtomicAdd(&IoPageCacheMappedPageCount, 1);
                if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
                    RtlAtomicAdd(&IoPageCacheMappedDirtyPageCount, 1);
                }
            }
        }
    }

    //
    // Now link the two entries based on their types. Note that nothing should
    // have been able to sneak in and link them since the caller has a
    // reference on both entries.
    //

    ASSERT(UpperEntry->BackingEntry == NULL);

    IoPageCacheEntryAddReference(LowerEntry);
    UpperEntry->BackingEntry = LowerEntry;
    Result = TRUE;

LinkPageCacheEntriesEnd:

    //
    // If the physical page removed from the entry was mapped, unmap it.
    //

    if (VirtualAddress != NULL) {
        PageSize = MmPageSize();
        MmUnmapAddress(VirtualAddress, PageSize);
    }

    //
    // If a physical page was removed from the entry, free it.
    //

    if (PhysicalAddress != INVALID_PHYSICAL_ADDRESS) {
        MmFreePhysicalPage(PhysicalAddress);
        RtlAtomicAdd(&IoPageCachePhysicalPageCount, (UINTN)-1);
    }

    return Result;
}

VOID
IopTrimPageCache (
    BOOL TimidEffort
    )

/*++

Routine Description:

    This routine removes as many clean page cache entries as is necessary to
    bring the size of the page cache back down to a reasonable level. It evicts
    the page cache entries in LRU order.

Arguments:

    TimidEffort - Supplies a boolean indicating whether or not this function
        should only try once to acquire a file object lock before moving on.
        Set this to TRUE if this thread might already be holding file object
        locks.

Return Value:

    None.

--*/

{

    LIST_ENTRY DestroyListHead;
    UINTN FreePageTarget;
    UINTN FreePhysicalPages;
    UINTN PageOutCount;
    UINTN TargetRemoveCount;

    TargetRemoveCount = 0;
    FreePhysicalPages = -1;
    if (IopIsPageCacheTooBig(&FreePhysicalPages) == FALSE) {
        goto TrimPageCacheEnd;
    }

    //
    // The page cache is not leaving enough free physical pages; determine how
    // many entries must be evicted.
    //

    ASSERT(FreePhysicalPages < IoPageCacheHeadroomPagesRetreat);

    TargetRemoveCount = IoPageCacheHeadroomPagesRetreat -
                        FreePhysicalPages;

    if (TargetRemoveCount > IoPageCachePhysicalPageCount) {
        TargetRemoveCount = IoPageCachePhysicalPageCount;
    }

    if (IoPageCachePhysicalPageCount - TargetRemoveCount <
        IoPageCacheMinimumPages) {

        TargetRemoveCount = IoPageCachePhysicalPageCount -
                            IoPageCacheMinimumPages;
    }

    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_SIZE_MANAGEMENT) != 0) {
        RtlDebugPrint("PAGE CACHE: Attempt to remove at least %lu entries.\n",
                      TargetRemoveCount);
    }

    //
    // Iterate over the clean LRU page cache list trying to find which page
    // cache entries can be removed. Stop as soon as the target count has been
    // reached.
    //

    INITIALIZE_LIST_HEAD(&DestroyListHead);
    if (!LIST_EMPTY(&IoPageCacheCleanUnmappedList)) {
        IopRemovePageCacheEntriesFromList(&IoPageCacheCleanUnmappedList,
                                          &DestroyListHead,
                                          TimidEffort,
                                          &TargetRemoveCount);
    }

    if (TargetRemoveCount != 0) {
        IopRemovePageCacheEntriesFromList(&IoPageCacheCleanList,
                                          &DestroyListHead,
                                          TimidEffort,
                                          &TargetRemoveCount);
    }

    //
    // Destroy the evicted page cache entries. This will reduce the page
    // cache's physical page count for any page that it ends up releasing.
    //

    IopDestroyPageCacheEntries(&DestroyListHead);

TrimPageCacheEnd:

    //
    // Also unmap things if the remaining page cache is causing too much
    // virtual memory pressure.
    //

    IopTrimPageCacheVirtual(TimidEffort);

    //
    // If the page cache is smaller than its target, ask MM to page out some
    // things so the page cache can grow back up to its target. This throws
    // pageable data into the mix, so if a process allocates a boatload of
    // memory, the page cache doesn't shrink to a dot and constantly lose the
    // working set of the process.
    //

    if ((TargetRemoveCount != 0) &&
        (IoPageCachePhysicalPageCount < IoPageCacheMinimumPagesTarget)) {

        PageOutCount = IoPageCacheMinimumPagesTarget -
                       IoPageCachePhysicalPageCount;

        FreePageTarget = FreePhysicalPages + PageOutCount;
        if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_SIZE_MANAGEMENT) != 0) {
            RtlDebugPrint("PAGE CACHE: Requesting page out: 0x%I64x\n",
                          PageOutCount);
        }

        MmRequestPagingOut(FreePageTarget);
    }

    return;
}

BOOL
IopIsPageCacheTooDirty (
    VOID
    )

/*++

Routine Description:

    This routine determines if the page cache has an uncomfortable number of
    entries in it that are dirty. Dirty entries are dangerous because they
    prevent the page cache from shrinking if memory gets tight.

Arguments:

    None.

Return Value:

    TRUE if the page cache has too many dirty entries and adding new ones
    should generally be avoided.

    FALSE if the page cache is relatively clean.

--*/

{

    UINTN FreePages;
    UINTN IdealSize;
    UINTN MaxDirty;
    BOOL TooDirty;

    //
    // The page cache thread is allowed to make all the pages dirty.
    //

    TooDirty = FALSE;
    if (KeGetCurrentThread() == IoPageCacheThread) {
        TooDirty = IopIsPageCacheTooBig(NULL);
        goto IsPageCacheTooDirtyEnd;
    }

    //
    // Determine the ideal page cache size.
    //

    FreePages = MmGetTotalFreePhysicalPages();
    if (FreePages < IoPageCacheHeadroomPagesRetreat) {
        IdealSize = IoPageCachePhysicalPageCount -
                    (IoPageCacheHeadroomPagesRetreat - FreePages);

    } else {
        IdealSize = IoPageCachePhysicalPageCount +
                    (FreePages - IoPageCacheHeadroomPagesRetreat);
    }

    //
    // Only a portion of that ideal size should be dirty.
    //

    MaxDirty = IdealSize >> PAGE_CACHE_MAX_DIRTY_SHIFT;
    if (IoPageCacheDirtyPageCount >= MaxDirty) {
        TooDirty = TRUE;
        goto IsPageCacheTooDirtyEnd;
    }

IsPageCacheTooDirtyEnd:
    return TooDirty;
}

COMPARISON_RESULT
IopComparePageCacheEntries (
    PRED_BLACK_TREE Tree,
    PRED_BLACK_TREE_NODE FirstNode,
    PRED_BLACK_TREE_NODE SecondNode
    )

/*++

Routine Description:

    This routine compares two Red-Black tree nodes contained inside file
    objects.

Arguments:

    Tree - Supplies a pointer to the Red-Black tree that owns both nodes.

    FirstNode - Supplies a pointer to the left side of the comparison.

    SecondNode - Supplies a pointer to the second side of the comparison.

Return Value:

    Same if the two nodes have the same value.

    Ascending if the first node is less than the second node.

    Descending if the second node is less than the first node.

--*/

{

    PPAGE_CACHE_ENTRY FirstEntry;
    PPAGE_CACHE_ENTRY SecondEntry;

    FirstEntry = RED_BLACK_TREE_VALUE(FirstNode, PAGE_CACHE_ENTRY, Node);
    SecondEntry = RED_BLACK_TREE_VALUE(SecondNode, PAGE_CACHE_ENTRY, Node);
    if (FirstEntry->Offset < SecondEntry->Offset) {
        return ComparisonResultAscending;

    } else if (FirstEntry->Offset > SecondEntry->Offset) {
        return ComparisonResultDescending;
    }

    return ComparisonResultSame;
}

//
// --------------------------------------------------------- Internal Functions
//

PPAGE_CACHE_ENTRY
IopCreatePageCacheEntry (
    PFILE_OBJECT FileObject,
    PVOID VirtualAddress,
    PHYSICAL_ADDRESS PhysicalAddress,
    ULONGLONG Offset
    )

/*++

Routine Description:

    This routine creates a page cache entry.

Arguments:

    FileObject - Supplies a pointer to the file object for the device or file
        that owns the page.

    VirtualAddress - Supplies an optional virtual address for the page.

    PhysicalAddress - Supplies the physical address of the page.

    Offset - Supplies the offset into the file or device where the page is
        from.

Return Value:

    Returns a pointer to a page cache entry on success, or NULL on failure.

--*/

{

    PPAGE_CACHE_ENTRY PageCacheEntry;

    ASSERT(IS_ALIGNED(PhysicalAddress, MmPageSize()) != FALSE);
    ASSERT((FileObject->Properties.Type != IoObjectBlockDevice) ||
           (Offset < (FileObject->Properties.BlockSize *
                      FileObject->Properties.BlockCount)));

    //
    // Allocate and initialize a new page cache entry.
    //

    PageCacheEntry = MmAllocateBlock(IoPageCacheBlockAllocator, NULL);
    if (PageCacheEntry == NULL) {
        goto CreatePageCacheEntryEnd;
    }

    RtlZeroMemory(PageCacheEntry, sizeof(PAGE_CACHE_ENTRY));
    IopFileObjectAddReference(FileObject);
    PageCacheEntry->FileObject = FileObject;
    PageCacheEntry->Offset = Offset;
    PageCacheEntry->PhysicalAddress = PhysicalAddress;
    if (VirtualAddress != NULL) {
        if (IoPageCacheDisableVirtualAddresses == FALSE) {
            PageCacheEntry->VirtualAddress = VirtualAddress;
        }
    }

    PageCacheEntry->ReferenceCount = 1;

CreatePageCacheEntryEnd:
    return PageCacheEntry;
}

VOID
IopDestroyPageCacheEntries (
    PLIST_ENTRY ListHead
    )

/*++

Routine Description:

    This routine destroys (or attempts to destroy) a list of page cache entries.
    Page cache entries that are not successfully destroyed will be marked
    evicted and put back on the global removal list for destruction later.

Arguments:

    ListHead - Supplies a pointer to the head of the list of entries to
        destroy.

Return Value:

    None. All page cache entries on this list are removed and either destroyed
    or put back on the global removal list for destruction later.

--*/

{

    PLIST_ENTRY CurrentEntry;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    UINTN RemovedCount;

    ASSERT(KeGetRunLevel() == RunLevelLow);

    RemovedCount = 0;
    while (LIST_EMPTY(ListHead) == FALSE) {
        CurrentEntry = ListHead->Next;
        PageCacheEntry = LIST_VALUE(CurrentEntry, PAGE_CACHE_ENTRY, ListEntry);
        LIST_REMOVE(CurrentEntry);
        CurrentEntry->Next = NULL;

        ASSERT(PageCacheEntry->ReferenceCount == 0);
        ASSERT(PageCacheEntry->Node.Parent == NULL);

        if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_EVICTION) != 0) {
            RtlDebugPrint("PAGE CACHE: Destroy entry 0x%08x: file object "
                          "0x%08x, offset 0x%I64x, physical address 0x%I64x, "
                          "reference count %d, flags 0x%08x.\n",
                          PageCacheEntry,
                          PageCacheEntry->FileObject,
                          PageCacheEntry->Offset,
                          PageCacheEntry->PhysicalAddress,
                          PageCacheEntry->ReferenceCount,
                          PageCacheEntry->Flags);
        }

        IopDestroyPageCacheEntry(PageCacheEntry);
        RemovedCount += 1;
    }

    //
    // Notify the debugger if any page cache entries were destroyed or failed
    // to be destroyed.
    //

    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_SIZE_MANAGEMENT) != 0) {
        if (RemovedCount != 0) {
            RtlDebugPrint("PAGE CACHE: Removed %d entries.\n", RemovedCount);
        }
    }

    return;
}

VOID
IopDestroyPageCacheEntry (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine destroys the given page cache entry. It is assumed that the
    page cache entry has already been removed from the cache and that is it
    not dirty.

Arguments:

    PageCacheEntry - Supplies a pointer to the page cache entry.

Return Value:

    None.

--*/

{

    PPAGE_CACHE_ENTRY BackingEntry;
    PFILE_OBJECT FileObject;
    ULONG PageSize;

    FileObject = PageCacheEntry->FileObject;

    ASSERT((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0);
    ASSERT((PageCacheEntry->ReferenceCount == 0) ||
           (PageCacheEntry->Node.Parent == NULL));

    //
    // If this is the page owner, then free the physical page.
    //

    if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0) {
        if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {

            ASSERT(PageCacheEntry->VirtualAddress != NULL);

            PageSize = MmPageSize();
            MmUnmapAddress(PageCacheEntry->VirtualAddress, PageSize);
            RtlAtomicAdd(&IoPageCacheMappedPageCount, (UINTN)-1);
            RtlAtomicAnd32(&(PageCacheEntry->Flags),
                           ~PAGE_CACHE_ENTRY_FLAG_MAPPED);

            PageCacheEntry->VirtualAddress = NULL;
        }

        MmFreePhysicalPage(PageCacheEntry->PhysicalAddress);
        RtlAtomicAdd(&IoPageCachePhysicalPageCount, (UINTN)-1);
        PageCacheEntry->PhysicalAddress = INVALID_PHYSICAL_ADDRESS;

    //
    // Otherwise release the reference on the page cache owner if it exists.
    //

    } else if (PageCacheEntry->BackingEntry != NULL) {
        BackingEntry = PageCacheEntry->BackingEntry;

        //
        // The virtual address must either be NULL or match the backing entry's
        // virtual address. It should never be the case that the backing entry
        // is not mapped while the non-backing entry is mapped.
        //

        ASSERT((PageCacheEntry->VirtualAddress == NULL) ||
               (PageCacheEntry->VirtualAddress ==
                BackingEntry->VirtualAddress));

        ASSERT(PageCacheEntry->PhysicalAddress ==
               BackingEntry->PhysicalAddress);

        IoPageCacheEntryReleaseReference(BackingEntry);
        PageCacheEntry->BackingEntry = NULL;
    }

    //
    // Release the reference on the file object.
    //

    IopFileObjectReleaseReference(FileObject);

    ASSERT((PageCacheEntry->ReferenceCount == 0) &&
           (PageCacheEntry->Node.Parent == NULL));

    //
    // With the final reference gone, free the page cache entry.
    //

    MmFreeBlock(IoPageCacheBlockAllocator, PageCacheEntry);
    return;
}

VOID
IopInsertPageCacheEntry (
    PPAGE_CACHE_ENTRY NewEntry,
    PPAGE_CACHE_ENTRY LinkEntry
    )

/*++

Routine Description:

    This routine inserts the new page cache entry into the page cache and links
    it to the link entry once it is inserted. This routine assumes that the
    page cache tree lock is held exclusively and that there is not already an
    entry for the same file and offset in the tree.

Arguments:

    NewEntry - Supplies a pointer to the new entry to insert into the page
        cache.

    LinkEntry - Supplies an optional pointer to an existing page cache entry
        to link to the page cache entry.

Return Value:

    None.

--*/

{

    ULONG ClearFlags;
    IO_OBJECT_TYPE LinkType;
    IO_OBJECT_TYPE NewType;
    ULONG OldFlags;
    PVOID VirtualAddress;

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(NewEntry->FileObject->Lock));
    ASSERT(NewEntry->Flags == 0);

    //
    // Insert the new entry into its file object's tree.
    //

    RtlRedBlackTreeInsert(&(NewEntry->FileObject->PageCacheTree),
                          &(NewEntry->Node));

    IoPageCacheEntryCount += 1;

    //
    // Now link the new entry to the supplied link entry based on their I/O
    // types.
    //

    if (LinkEntry != NULL) {
        LinkType = LinkEntry->FileObject->Properties.Type;
        NewType = NewEntry->FileObject->Properties.Type;

        ASSERT(LinkType != NewType);
        ASSERT(IS_IO_OBJECT_TYPE_LINKABLE(LinkType) != FALSE);
        ASSERT(IS_IO_OBJECT_TYPE_LINKABLE(NewType) != FALSE);
        ASSERT((LinkEntry->Flags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0);
        ASSERT(LinkEntry->PhysicalAddress == NewEntry->PhysicalAddress);
        ASSERT(LinkEntry->VirtualAddress == NewEntry->VirtualAddress);

        if ((LinkType == IoObjectBlockDevice) &&
            (IO_IS_CACHEABLE_FILE(NewType))) {

            IoPageCacheEntryAddReference(LinkEntry);
            NewEntry->BackingEntry = LinkEntry;

        } else {

            ASSERT((IO_IS_CACHEABLE_FILE(LinkType)) &&
                   (NewType == IoObjectBlockDevice));

            IoPageCacheEntryAddReference(NewEntry);
            LinkEntry->BackingEntry = NewEntry;
            ClearFlags = PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER |
                         PAGE_CACHE_ENTRY_FLAG_MAPPED;

            OldFlags = RtlAtomicAnd32(&(LinkEntry->Flags), ~ClearFlags);

            //
            // The link entry had better not be dirty, because then it would be
            // a dirty non-page-owner entry, which messes up the accounting.
            //

            ASSERT((OldFlags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0);

            NewEntry->Flags = PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER;

            //
            // If the old entry was mapped, it better be the same mapping as
            // the new entry (if any), since otherwise the new entry VA would
            // be leaked.
            //

            if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
                VirtualAddress = LinkEntry->VirtualAddress;

                ASSERT((VirtualAddress != NULL) &&
                       ((NewEntry->VirtualAddress == NULL) ||
                        (NewEntry->VirtualAddress == VirtualAddress)));

                NewEntry->VirtualAddress = VirtualAddress;
                NewEntry->Flags |= PAGE_CACHE_ENTRY_FLAG_MAPPED;
            }
        }

    } else {
        if (NewEntry->VirtualAddress != NULL) {
            NewEntry->Flags |= PAGE_CACHE_ENTRY_FLAG_MAPPED;
            RtlAtomicAdd(&IoPageCacheMappedPageCount, 1);
        }

        RtlAtomicAdd(&IoPageCachePhysicalPageCount, 1);
        NewEntry->Flags |= PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER;
        MmSetPageCacheEntryForPhysicalAddress(NewEntry->PhysicalAddress,
                                              NewEntry);
    }

    return;
}

PPAGE_CACHE_ENTRY
IopLookupPageCacheEntryHelper (
    PFILE_OBJECT FileObject,
    ULONGLONG Offset
    )

/*++

Routine Description:

    This routine searches for a page cache entry based on the file object and
    offset. This routine assumes the page cache lock is held. If found, this
    routine takes a reference on the page cache entry.

Arguments:

    FileObject - Supplies a pointer to the file object for the file or device.

    Offset - Supplies an offset into the file or device.

Return Value:

    Returns a pointer to the found page cache entry on success, or NULL on
    failure.

--*/

{

    PPAGE_CACHE_ENTRY FoundEntry;
    PRED_BLACK_TREE_NODE FoundNode;
    PAGE_CACHE_ENTRY SearchEntry;

    SearchEntry.FileObject = FileObject;
    SearchEntry.Offset = Offset;
    SearchEntry.Flags = 0;
    FoundNode = RtlRedBlackTreeSearch(&(FileObject->PageCacheTree),
                                      &(SearchEntry.Node));

    if (FoundNode == NULL) {
        return NULL;
    }

    FoundEntry = RED_BLACK_TREE_VALUE(FoundNode, PAGE_CACHE_ENTRY, Node);
    IoPageCacheEntryAddReference(FoundEntry);
    return FoundEntry;
}

VOID
IopPageCacheThread (
    PVOID Parameter
    )

/*++

Routine Description:

    This routine cleans cached pages and removes clean pages if the cache is
    consuming too much memory.

Arguments:

    Parameter - Supplies a pointer supplied by the creator of the thread. This
        parameter is not used.

Return Value:

    None.

--*/

{

    ULONGLONG CurrentTime;
    PKEVENT PhysicalMemoryWarningEvent;
    PVOID SignalingObject;
    KSTATUS Status;
    PKEVENT VirtualMemoryWarningEvent;
    PVOID WaitObjectArray[3];

    Status = STATUS_SUCCESS;
    IoPageCacheThread = KeGetCurrentThread();

    //
    // Get the memory warning events from the memory manager.
    //

    PhysicalMemoryWarningEvent = MmGetPhysicalMemoryWarningEvent();
    VirtualMemoryWarningEvent = MmGetVirtualMemoryWarningEvent();

    ASSERT(PhysicalMemoryWarningEvent != NULL);
    ASSERT(VirtualMemoryWarningEvent != NULL);

    //
    // There are only three objects to wait for and as this is less than the
    // thread's built-in wait blocks, do not pre-allocate a wait block.
    //

    ASSERT(3 < BUILTIN_WAIT_BLOCK_ENTRY_COUNT);

    WaitObjectArray[0] = IoPageCacheWorkTimer;
    WaitObjectArray[1] = PhysicalMemoryWarningEvent;
    WaitObjectArray[2] = VirtualMemoryWarningEvent;

    //
    // Loop forever waiting for either the page cache timer or the memory
    // manager's warning event.
    //

    while (TRUE) {
        Status = ObWaitOnObjects(WaitObjectArray,
                                 3,
                                 0,
                                 WAIT_TIME_INDEFINITE,
                                 NULL,
                                 &SignalingObject);

        ASSERT(KSUCCESS(Status));

        //
        // The page cache cleaning is about to start. Mark down the current
        // time as the last time the cleaning ran. The leaves a record that an
        // attempt was made to flush any writes that occurred before this time.
        //

        CurrentTime = KeGetRecentTimeCounter();
        WRITE_INT64_SYNC(&IoLastPageCacheCleanTime, CurrentTime);

        //
        // Loop over the process of removing excess entries and flushing
        // dirty entries. The flush code may decided to loop back and remove
        // more excess entries.
        //

        while (TRUE) {
            Status = STATUS_SUCCESS;

            //
            // Blast away the list of page cache entries that are ready for
            // removal.
            //

            IopTrimRemovalPageCacheList();

            //
            // Attempt to trim out some clean page cache entries from the LRU
            // list. This routine should only do any work if memory is tight.
            // This is the root of the page cache thread, so there's never
            // recursive I/O to worry about (so go ahead and destroy file
            // objects).
            //

            IopTrimPageCache(FALSE);

            //
            // Flush some dirty file objects.
            //

            Status = IopFlushFileObjects(0, 0, NULL);
            if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_DIRTY_LISTS) != 0) {
                IopCheckDirtyFileObjectsList();
            }

            if (Status == STATUS_TRY_AGAIN) {
                continue;
            }

            //
            // If the page cache appears to be completely clean, try to kill the
            // timer and go dormant. Kill the timer, change the state to clean,
            // and then see if any dirtiness snuck in while that was happening.
            // If so, set it back to dirty (racing with everyone else that
            // may have already done that).
            //

            KeCancelTimer(IoPageCacheWorkTimer);
            RtlAtomicExchange32(&IoPageCacheState, PageCacheStateClean);
            if ((!LIST_EMPTY(&IoFileObjectsDirtyList)) ||
                (IoPageCacheDirtyPageCount != 0)) {

                IopSchedulePageCacheThread();
            }

            break;
        }
    }

    return;
}

KSTATUS
IopFlushPageCacheBuffer (
    PIO_BUFFER FlushBuffer,
    UINTN FlushSize,
    ULONG Flags
    )

/*++

Routine Description:

    This routine flushes the given buffer to the owning file or device. This
    routine assumes that the lock of the file object that owns the page cache
    entries is held in the appropriate mode.

Arguments:

    FlushBuffer - Supplies a pointer to a cache-backed I/O buffer to flush.

    FlushSize - Supplies the number of bytes to flush.

    Flags - Supplies a bitmaks of I/O flags for the flush. See IO_FLAG_* for
        definitions.

Return Value:

    Status code.

--*/

{

    UINTN BufferOffset;
    UINTN BytesToWrite;
    PPAGE_CACHE_ENTRY CacheEntry;
    BOOL Clean;
    PFILE_OBJECT FileObject;
    ULONGLONG FileOffset;
    ULONGLONG FileSize;
    IO_CONTEXT IoContext;
    BOOL MarkedClean;
    ULONG PageSize;
    KSTATUS Status;

    CacheEntry = MmGetIoBufferPageCacheEntry(FlushBuffer, 0);
    FileObject = CacheEntry->FileObject;
    FileOffset = CacheEntry->Offset;
    PageSize = MmPageSize();
    READ_INT64_SYNC(&(FileObject->Properties.FileSize), &FileSize);

    ASSERT(FlushSize <= PAGE_CACHE_FLUSH_MAX);
    ASSERT(KeIsSharedExclusiveLockHeld(FileObject->Lock) != FALSE);

    //
    // Try to mark all the pages clean. If they are all already clean, then
    // just exit. Something is already performing the I/O. Also make sure that
    // all the supplied page cache entries are still in the cache. If an
    // evicted entry is found, do not write any data from that page or further;
    // the file was truncated.
    //

    BufferOffset = 0;
    BytesToWrite = 0;
    Clean = TRUE;
    while (BufferOffset < FlushSize) {
        CacheEntry = MmGetIoBufferPageCacheEntry(FlushBuffer, BufferOffset);
        if (CacheEntry->Node.Parent == NULL) {
            break;
        }

        //
        // Evicted entries should never be flushed.
        //

        ASSERT(CacheEntry->Node.Parent != NULL);

        MarkedClean = IopMarkPageCacheEntryClean(CacheEntry, TRUE);
        if (MarkedClean != FALSE) {
            Clean = FALSE;
        }

        BytesToWrite += PageSize;
        BufferOffset += PageSize;
    }

    //
    // Avoid writing beyond the end of the file.
    //

    if (FileOffset + BytesToWrite > FileSize) {

        ASSERT(FileOffset <= FileSize);

        BytesToWrite = FileSize - FileOffset;
    }

    //
    // If there are no bytes to write, because all the pages got evicted, then
    // exit now.
    //

    if (BytesToWrite == 0) {
        Status = STATUS_SUCCESS;
        goto FlushPageCacheBufferEnd;
    }

    //
    // Exit now if it was already clean, unless this is synchronized I/O. It
    // could be that the backing entries are what require flushing and this
    // layer does not have jurisdiction to mark them clean.
    //

    if ((Clean != FALSE) && ((Flags & IO_FLAG_DATA_SYNCHRONIZED) == 0)) {
        Status = STATUS_SUCCESS;
        goto FlushPageCacheBufferEnd;
    }

    IoContext.IoBuffer = FlushBuffer;
    IoContext.Offset = FileOffset;
    IoContext.SizeInBytes = BytesToWrite;
    IoContext.Flags = Flags;
    IoContext.TimeoutInMilliseconds = WAIT_TIME_INDEFINITE;
    IoContext.Write = TRUE;
    Status = IopPerformNonCachedWrite(FileObject, &IoContext, NULL);
    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_FLUSH) != 0) {
        if ((!KSUCCESS(Status)) || (Flags != 0) ||
            (IoContext.BytesCompleted != BytesToWrite)) {

            RtlDebugPrint("PAGE CACHE: Flushed FILE_OBJECT 0x%08x "
                          "with status 0x%08x: flags 0x%x, file offset "
                          "0x%I64x, bytes attempted 0x%x, bytes completed "
                          "0x%x.\n",
                          FileObject,
                          Status,
                          Flags,
                          FileOffset,
                          BytesToWrite,
                          IoContext.BytesCompleted);

        } else {
            RtlDebugPrint("PAGE CACHE: Flushed FILE_OBJECT %x "
                          "Offset 0x%I64x Size 0x%x\n",
                          FileObject,
                          FileOffset,
                          BytesToWrite);
        }
    }

    if (!KSUCCESS(Status))  {
        goto FlushPageCacheBufferEnd;
    }

    if (IoContext.BytesCompleted != BytesToWrite) {
        Status = STATUS_DATA_LENGTH_MISMATCH;
        goto FlushPageCacheBufferEnd;
    }

    Status = STATUS_SUCCESS;

FlushPageCacheBufferEnd:
    if (!KSUCCESS(Status)) {

        //
        // Mark the non-written pages as dirty again.
        //

        BufferOffset = ALIGN_RANGE_DOWN(IoContext.BytesCompleted, PageSize);
        while (BufferOffset < BytesToWrite) {
            CacheEntry = MmGetIoBufferPageCacheEntry(FlushBuffer, BufferOffset);
            IopMarkPageCacheEntryDirty(CacheEntry);
            BufferOffset += PageSize;
        }

        if (IoContext.BytesCompleted != BytesToWrite) {
            IopMarkFileObjectDirty(CacheEntry->FileObject);
        }
    }

    return Status;
}

VOID
IopTrimRemovalPageCacheList (
    VOID
    )

/*++

Routine Description:

    This routine removes the page cache entries from the list of page cache
    entries that are ready for removal.

Arguments:

    None.

Return Value:

    None.

--*/

{

    LIST_ENTRY DestroyListHead;

    if (LIST_EMPTY(&(IoPageCacheRemovalList)) != FALSE) {
        return;
    }

    INITIALIZE_LIST_HEAD(&DestroyListHead);
    IopRemovePageCacheEntriesFromList(&IoPageCacheRemovalList,
                                      &DestroyListHead,
                                      FALSE,
                                      NULL);

    //
    // Destroy the evicted page cache entries. This will reduce the page
    // cache's physical page count for any page that it ends up releasing.
    //

    IopDestroyPageCacheEntries(&DestroyListHead);
    return;
}

VOID
IopRemovePageCacheEntriesFromList (
    PLIST_ENTRY PageCacheListHead,
    PLIST_ENTRY DestroyListHead,
    BOOL TimidEffort,
    PUINTN TargetRemoveCount
    )

/*++

Routine Description:

    This routine processes page cache entries in the given list, removing them
    from the tree and the list, if possible. If a target remove count is
    supplied, then the removal process will stop as soon as the removal count
    reaches 0 or the end of the list is reached. This routine assumes that the
    page cache tree lock is held exclusively.

Arguments:

    PageCacheListHead - Supplies a pointer to the head of the page cache list.

    DestroyListHead - Supplies a pointer to the head of the list of page cache
        entries that can be destroyed as a result of the removal process.

    TimidEffort - Supplies a boolean indicating whether or not this function
        should only try once to acquire a file object lock before moving on.
        Set this to TRUE if this thread might already be holding file object
        locks.

    TargetRemoveCount - Supplies an optional pointer to the number of page
        cache entries the caller wishes to remove from the list. On return, it
        will store the difference between the target and the actual number of
        page cache entries removed. If not supplied, then the routine will
        process the entire list looking for page cache entries to remove.

Return Value:

    None.

--*/

{

    PFILE_OBJECT FileObject;
    ULONG Flags;
    LIST_ENTRY LocalList;
    PSHARED_EXCLUSIVE_LOCK Lock;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    BOOL PageTakenDown;
    BOOL PageWasDirty;
    KSTATUS Status;

    KeAcquireQueuedLock(IoPageCacheListLock);
    if (LIST_EMPTY(PageCacheListHead)) {
        KeReleaseQueuedLock(IoPageCacheListLock);
        return;
    }

    //
    // Move the contents of the list over to a local list to avoid infinitely
    // working on the same entries. The local list is also protected by the
    // list lock, and cannot be manipulated without it.
    //

    MOVE_LIST(PageCacheListHead, &LocalList);
    INITIALIZE_LIST_HEAD(PageCacheListHead);
    while ((!LIST_EMPTY(&LocalList)) &&
           ((TargetRemoveCount == NULL) || (*TargetRemoveCount != 0))) {

        PageCacheEntry = LIST_VALUE(LocalList.Next,
                                    PAGE_CACHE_ENTRY,
                                    ListEntry);

        FileObject = PageCacheEntry->FileObject;
        Flags = PageCacheEntry->Flags;

        //
        // Remove anything with a reference to avoid iterating through it
        // over and over. When that last reference is dropped, it will be put
        // back on.
        //

        if (PageCacheEntry->ReferenceCount != 0) {
            LIST_REMOVE(&(PageCacheEntry->ListEntry));
            PageCacheEntry->ListEntry.Next = NULL;

            //
            // Double check the reference count. If it dropped to zero while
            // the entry was being removed, it may not have observed the list
            // entry being nulled out, and may not be waiting to put the entry
            // back.
            //

            RtlMemoryBarrier();
            if (PageCacheEntry->ReferenceCount == 0) {
                INSERT_BEFORE(&(PageCacheEntry->ListEntry),
                              &IoPageCacheCleanList);
            }

            continue;
        }

        //
        // If it's dirty, then there must be another thread that just marked it
        // dirty but has yet to remove it from the list. Remove it and move on.
        //

        if ((Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
            LIST_REMOVE(&(PageCacheEntry->ListEntry));
            PageCacheEntry->ListEntry.Next = NULL;
            continue;
        }

        //
        // For timid attempts, try to get the lock without dropping the list
        // lock (since for a single attempt lock inversions are not an issue).
        // If it fails, just move on in case this thread already owns the lock
        // in question further up the stack.
        //

        Lock = FileObject->Lock;
        if (TimidEffort != FALSE) {
            if (KeTryToAcquireSharedExclusiveLockExclusive(Lock) == FALSE) {
                LIST_REMOVE(&(PageCacheEntry->ListEntry));
                INSERT_BEFORE(&(PageCacheEntry->ListEntry),
                              &IoPageCacheCleanList);

                continue;
            }
        }

        //
        // Add a reference to the entry, drop the list lock, and acquire the
        // file object lock to prevent lock ordering trouble.
        //

        IoPageCacheEntryAddReference(PageCacheEntry);
        KeReleaseQueuedLock(IoPageCacheListLock);

        //
        // Acquire the lock if not already acquired.
        //

        if (TimidEffort == FALSE) {
            KeAcquireSharedExclusiveLockExclusive(Lock);
        }

        PageTakenDown = FALSE;
        if (PageCacheEntry->ReferenceCount == 1) {

            //
            // Unmap this page cache entry from any image sections that may be
            // mapping it. If the mappings note that the page is dirty, then
            // mark it dirty and skip removing the page if it became dirty. The
            // file object lock holds off any new mappings from getting at this
            // entry. Unmapping a page cache entry can fail if a non-paged
            // image section maps it.
            //

            Flags = PageCacheEntry->Flags;
            Status = IopUnmapPageCacheEntrySections(PageCacheEntry,
                                                    &PageWasDirty);

            if (KSUCCESS(Status)) {
                if (PageWasDirty != FALSE) {
                    IopMarkPageCacheEntryDirty(PageCacheEntry);
                    Flags = PageCacheEntry->Flags;
                }

                if ((Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) {

                    //
                    // Make sure the cache entry is clean to keep the metrics
                    // correct.
                    //

                    IopMarkPageCacheEntryClean(PageCacheEntry, FALSE);

                    //
                    // Remove the node for the page cache entry tree if
                    // necessary.
                    //

                    if (PageCacheEntry->Node.Parent != NULL) {
                        IopRemovePageCacheEntryFromTree(PageCacheEntry);
                    }

                    PageTakenDown = TRUE;

                    //
                    // If this page cache entry owns its physical page, then it
                    // counts towards the removal count.
                    //

                    if ((Flags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0) {
                        if (TargetRemoveCount != NULL) {
                            *TargetRemoveCount -= 1;
                        }
                    }
                }
            }
        }

        //
        // Drop the file object lock and reacquire the list lock.
        //

        KeReleaseSharedExclusiveLockExclusive(FileObject->Lock);
        KeAcquireQueuedLock(IoPageCacheListLock);

        //
        // If the page was successfully destroyed, move it over to the destroy
        // list.
        //

        if (PageTakenDown != FALSE) {

            ASSERT((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0);

            if (PageCacheEntry->ListEntry.Next != NULL) {
                LIST_REMOVE(&(PageCacheEntry->ListEntry));
            }

            INSERT_BEFORE(&(PageCacheEntry->ListEntry), DestroyListHead);

        //
        // Otherwise, either remove it from this list, or stick it on the
        // end. The list assignment has to be correct because releasing
        // the reference might try to stick it on the list if it sees its clean
        // and not on one, but the list lock is already held here.
        //

        } else {
            if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) {
                LIST_REMOVE(&(PageCacheEntry->ListEntry));
                INSERT_BEFORE(&(PageCacheEntry->ListEntry),
                              &IoPageCacheCleanList);
            }
        }

        IoPageCacheEntryReleaseReference(PageCacheEntry);
    }

    //
    // Stick any remainder back on list.
    //

    if (!LIST_EMPTY(&LocalList)) {
        APPEND_LIST(&LocalList, PageCacheListHead);
    }

    KeReleaseQueuedLock(IoPageCacheListLock);
    return;
}

VOID
IopTrimPageCacheVirtual (
    BOOL TimidEffort
    )

/*++

Routine Description:

    This routine unmaps as many clean page cache entries as is necessary to
    bring the number of mapped page cache entries back down to a reasonable
    level. It unmaps page cache entires in LRU order.

Arguments:

    TimidEffort - Supplies a boolean indicating whether or not this function
        should only try once to acquire a file object lock before moving on.
        Set this to TRUE if this thread might already be holding file object
        locks.

Return Value:

    None.

--*/

{

    PLIST_ENTRY CurrentEntry;
    PFILE_OBJECT FileObject;
    UINTN FreeVirtualPages;
    PSHARED_EXCLUSIVE_LOCK Lock;
    UINTN MappedCleanPageCount;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    ULONG PageSize;
    LIST_ENTRY ReturnList;
    UINTN TargetUnmapCount;
    UINTN UnmapCount;
    UINTN UnmapSize;
    PVOID UnmapStart;
    PVOID VirtualAddress;

    TargetUnmapCount = 0;
    FreeVirtualPages = -1;
    if ((LIST_EMPTY(&IoPageCacheCleanList)) ||
        (IopIsPageCacheTooMapped(&FreeVirtualPages) == FALSE)) {

        return;
    }

    ASSERT(FreeVirtualPages != -1);

    INITIALIZE_LIST_HEAD(&ReturnList);

    //
    // The page cache is not leaving enough free virtual memory; determine how
    // many entries must be unmapped.
    //

    TargetUnmapCount = 0;
    if (FreeVirtualPages < IoPageCacheHeadroomVirtualPagesRetreat) {
        TargetUnmapCount = IoPageCacheHeadroomVirtualPagesRetreat -
                           FreeVirtualPages;
    }

    //
    // Assert on the accounting numbers, but allow for a bit of transience.
    //

    ASSERT(IoPageCacheMappedDirtyPageCount <=
           IoPageCacheMappedPageCount + 0x10);

    ASSERT(IoPageCacheMappedDirtyPageCount <= IoPageCacheDirtyPageCount + 0x10);

    MappedCleanPageCount = IoPageCacheMappedPageCount -
                           IoPageCacheMappedDirtyPageCount;

    if (TargetUnmapCount > MappedCleanPageCount) {
        TargetUnmapCount = MappedCleanPageCount;
    }

    if (TargetUnmapCount == 0) {
        if (MmGetVirtualMemoryWarningLevel() == MemoryWarningLevelNone) {
            return;
        }

        //
        // Unmap some minimum number of pages before relying on the virtual
        // warning to indicate when the coast is clear. This should hopefully
        // build some headroom in fragmented cases.
        //

        TargetUnmapCount = IoPageCacheHeadroomVirtualPagesRetreat -
                           IoPageCacheHeadroomVirtualPagesTrigger;
    }

    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_MAPPED_MANAGEMENT) != 0) {
        RtlDebugPrint("PAGE CACHE: Attempt to unmap at least %lu entries.\n",
                      TargetUnmapCount);
    }

    //
    // Iterate over the clean LRU page cache list trying to unmap page cache
    // entries. Stop as soon as the target count has been reached.
    //

    UnmapStart = NULL;
    UnmapSize = 0;
    UnmapCount = 0;
    PageSize = MmPageSize();
    KeAcquireQueuedLock(IoPageCacheListLock);
    while ((!LIST_EMPTY(&IoPageCacheCleanList)) &&
           ((TargetUnmapCount != UnmapCount) ||
            (MmGetVirtualMemoryWarningLevel() != MemoryWarningLevelNone))) {

        CurrentEntry = IoPageCacheCleanList.Next;
        PageCacheEntry = LIST_VALUE(CurrentEntry, PAGE_CACHE_ENTRY, ListEntry);

        //
        // Skip over all page cache entries with references, removing them from
        // this list. They cannot be unmapped at the moment.
        //

        if (PageCacheEntry->ReferenceCount != 0) {
            LIST_REMOVE(&(PageCacheEntry->ListEntry));
            PageCacheEntry->ListEntry.Next = NULL;

            //
            // Double check the reference count. If it dropped to zero while
            // the entry was being removed, it may not have observed the list
            // entry being nulled out, and may not be waiting to put the entry
            // back.
            //

            RtlMemoryBarrier();
            if (PageCacheEntry->ReferenceCount == 0) {
                INSERT_BEFORE(&(PageCacheEntry->ListEntry),
                              &IoPageCacheCleanList);
            }

            continue;
        }

        //
        // If it's dirty, then there must be another thread that just marked it
        // dirty but has yet to remove it from the list. Remove it and move on.
        //

        if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
            LIST_REMOVE(&(PageCacheEntry->ListEntry));
            PageCacheEntry->ListEntry.Next = NULL;
            continue;
        }

        //
        // If the page was not mapped, move it over to the clean unmapped list
        // to prevent iterating over it again during subsequent invocations of
        // this function.
        //

        if (PageCacheEntry->VirtualAddress == NULL) {
            LIST_REMOVE(&(PageCacheEntry->ListEntry));
            INSERT_BEFORE(&(PageCacheEntry->ListEntry),
                          &IoPageCacheCleanUnmappedList);

            continue;
        }

        FileObject = PageCacheEntry->FileObject;
        Lock = FileObject->Lock;

        //
        // For timid attempts, try to get the lock without dropping the list
        // lock (since for a single attempt lock inversions are not an issue).
        // If it fails, just move on in case this thread already owns the lock
        // in question further up the stack.
        //

        if (TimidEffort != FALSE) {
            if (KeTryToAcquireSharedExclusiveLockExclusive(Lock) == FALSE) {
                LIST_REMOVE(&(PageCacheEntry->ListEntry));
                INSERT_BEFORE(&(PageCacheEntry->ListEntry), &ReturnList);
                continue;
            }
        }

        //
        // Add a reference to the page cache entry, drop the list lock, and
        // acquire the file object lock to ensure no new references come in
        // while the VA is being torn down.
        //

        IoPageCacheEntryAddReference(PageCacheEntry);
        KeReleaseQueuedLock(IoPageCacheListLock);
        if (TimidEffort == FALSE) {
            KeAcquireSharedExclusiveLockExclusive(Lock);
        }

        IopRemovePageCacheEntryVirtualAddress(PageCacheEntry, &VirtualAddress);
        if (VirtualAddress != NULL) {
            UnmapCount += 1;

            //
            // If this page is not contiguous with the previous run, unmap the
            // previous run.
            //

            if ((UnmapStart != NULL) &&
                (VirtualAddress != (UnmapStart + UnmapSize))) {

                MmUnmapAddress(UnmapStart, UnmapSize);
                UnmapStart = NULL;
                UnmapSize = 0;
            }

            //
            // Either start a new run or append it to the previous run.
            //

            if (UnmapStart == NULL) {
                UnmapStart = VirtualAddress;
            }

            UnmapSize += PageSize;
        }

        //
        // Drop the file object lock and reacquire the list lock.
        //

        KeReleaseSharedExclusiveLockExclusive(FileObject->Lock);
        KeAcquireQueuedLock(IoPageCacheListLock);
        if ((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) {
            if (PageCacheEntry->ListEntry.Next != NULL) {
                LIST_REMOVE(&(PageCacheEntry->ListEntry));
            }

            INSERT_BEFORE(&(PageCacheEntry->ListEntry),
                          &IoPageCacheCleanUnmappedList);
        }

        IoPageCacheEntryReleaseReference(PageCacheEntry);
    }

    //
    // Stick any entries whose locks couldn't be acquired back at the time
    // back on the list.
    //

    if (!LIST_EMPTY(&ReturnList)) {
        APPEND_LIST(&ReturnList, &IoPageCacheCleanList);
    }

    KeReleaseQueuedLock(IoPageCacheListLock);

    //
    // If there is a remaining region of contiguous virtual memory that needs
    // to be unmapped, it can be done after releasing the lock as all of the
    // page cache entries have already been updated to reflect being unmapped.
    //

    if (UnmapStart != NULL) {
        MmUnmapAddress(UnmapStart, UnmapSize);
    }

    if (UnmapCount != 0) {
        RtlAtomicAdd(&IoPageCacheMappedPageCount, -UnmapCount);
    }

    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_MAPPED_MANAGEMENT) != 0) {
        RtlDebugPrint("PAGE CACHE: Unmapped %lu entries.\n", UnmapCount);
    }

    return;
}

BOOL
IopIsIoBufferPageCacheBackedHelper (
    PFILE_OBJECT FileObject,
    PIO_BUFFER IoBuffer,
    ULONGLONG Offset,
    UINTN SizeInBytes
    )

/*++

Routine Description:

    This routine determines whether or not the given I/O buffer with data
    targeting the given file object at the given offset is currently backed by
    the page cache, up to the given size. The caller is expected to synchronize
    with eviction via truncate.

Arguments:

    FileObject - Supplies a pointer to a file object.

    IoBuffer - Supplies a pointer to an I/O buffer.

    Offset - Supplies an offset into the file or device object.

    SizeInBytes - Supplies the number of bytes in the I/O buffer that should be
        cache backed.

Return Value:

    Returns TRUE if the I/O buffer is backed by valid page cache entries, or
    FALSE otherwise.

--*/

{

    UINTN BufferOffset;
    PPAGE_CACHE_ENTRY PageCacheEntry;
    ULONG PageSize;

    PageSize = MmPageSize();

    ASSERT(IS_ALIGNED(SizeInBytes, PageSize) != FALSE);

    BufferOffset = 0;
    while (SizeInBytes != 0) {

        //
        // If this page in the buffer is not backed by a page cache entry or
        // not backed by the correct page cache entry, then return FALSE. Also
        // return FALSE if the offsets do not agree.
        //

        PageCacheEntry = MmGetIoBufferPageCacheEntry(IoBuffer, BufferOffset);
        if ((PageCacheEntry == NULL) ||
            (PageCacheEntry->FileObject != FileObject) ||
            (PageCacheEntry->Node.Parent == NULL) ||
            (PageCacheEntry->Offset != Offset)) {

            return FALSE;
        }

        SizeInBytes -= PageSize;
        BufferOffset += PageSize;
        Offset += PageSize;
    }

    return TRUE;
}

KSTATUS
IopUnmapPageCacheEntrySections (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    PBOOL PageWasDirty
    )

/*++

Routine Description:

    This routine unmaps the physical page owned by the given page cache entry
    from all the image sections that may have it mapped.

Arguments:

    PageCacheEntry - Supplies a pointer to the page cache entry to be unmapped.

    PageWasDirty - Supplies a pointer where a boolean will be returned
        indicating if the page that was unmapped was dirty. This parameter is
        optional.

Return Value:

    Status code.

--*/

{

    ULONG Flags;
    PIMAGE_SECTION_LIST ImageSectionList;
    KSTATUS Status;

    //
    // The page cache entry shouldn't be referenced by random I/O buffers
    // because they could add mappings after this work is done. A reference
    // count of 1 is accepted for link, which has a reference but isn't doing
    // anything wild with it.
    //

    ASSERT(PageCacheEntry->ReferenceCount <= 1);

    Status = STATUS_SUCCESS;
    if (PageWasDirty != NULL) {
        *PageWasDirty = FALSE;
    }

    ImageSectionList = PageCacheEntry->FileObject->ImageSectionList;
    if (ImageSectionList != NULL) {
        Flags = IMAGE_SECTION_UNMAP_FLAG_PAGE_CACHE_ONLY;
        Status = MmUnmapImageSectionList(ImageSectionList,
                                         PageCacheEntry->Offset,
                                         MmPageSize(),
                                         Flags,
                                         PageWasDirty);
    }

    return Status;
}

KSTATUS
IopRemovePageCacheEntryVirtualAddress (
    PPAGE_CACHE_ENTRY Entry,
    PVOID *VirtualAddress
    )

/*++

Routine Description:

    This routine attmepts to separate a page cache entry from its associated
    virtual address. It assumes the file object lock for this entry (but not
    the backing entry if there is one) is held.

Arguments:

    Entry - Supplies a pointer to the page cache entry.

    VirtualAddress - Supplies a pointer where the virtual address to unmap will
        be returned on success. NULL will be returned on failure or if there
        was no VA.

Return Value:

    STATUS_SUCCESS on success.

    STATUS_RESOURCE_IN_USE if the page cache entry has references and cannot
    be unmapped.

--*/

{

    PPAGE_CACHE_ENTRY BackingEntry;
    ULONG OldFlags;
    KSTATUS Status;

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(Entry->FileObject->Lock));

    Status = STATUS_RESOURCE_IN_USE;
    *VirtualAddress = NULL;
    BackingEntry = NULL;
    if ((Entry->ReferenceCount != 1) ||
        ((Entry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0)) {

        goto RemovePageCacheEntryVirtualAddressEnd;
    }

    //
    // If this page cache entry owns the physical page, then it is not
    // serving as a backing entry to any other page cache entry (as it has
    // no references). Freely unmap it.
    //

    if ((Entry->Flags & PAGE_CACHE_ENTRY_FLAG_PAGE_OWNER) != 0) {
        OldFlags = RtlAtomicAnd32(&(Entry->Flags),
                                  ~PAGE_CACHE_ENTRY_FLAG_MAPPED);

    //
    // The page cache entry is not the owner, but it may be eligible for
    // unmap if the owner only has 1 reference (from the backee).
    //

    } else {

        //
        // Grab the backing entry lock, too. Lock ordering shouldn't be a
        // problem since files are always grabbed before block devices.
        //

        BackingEntry = Entry->BackingEntry;

        ASSERT(BackingEntry != NULL);

        KeAcquireSharedExclusiveLockExclusive(BackingEntry->FileObject->Lock);

        ASSERT((Entry->VirtualAddress == NULL) ||
               (BackingEntry->VirtualAddress ==
                Entry->VirtualAddress));

        if ((BackingEntry->ReferenceCount != 1) ||
            ((BackingEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0)) {

            goto RemovePageCacheEntryVirtualAddressEnd;
        }

        //
        // Only the owner should be marked mapped or dirty.
        //

        ASSERT((Entry->Flags &
                (PAGE_CACHE_ENTRY_FLAG_MAPPED |
                 PAGE_CACHE_ENTRY_FLAG_DIRTY)) == 0);

        OldFlags = RtlAtomicAnd32(&(BackingEntry->Flags),
                                  ~PAGE_CACHE_ENTRY_FLAG_MAPPED);
    }

    if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_MAPPED) != 0) {
        if (BackingEntry != NULL) {
            *VirtualAddress = BackingEntry->VirtualAddress;
            BackingEntry->VirtualAddress = NULL;

        } else {
            *VirtualAddress = Entry->VirtualAddress;
        }

        Entry->VirtualAddress = NULL;

        //
        // If the unmapped page was also dirty, decrement the count. The
        // mapped page count is not decremented because it's assumed the caller
        // will do that (potentially in bulk).
        //

        if ((OldFlags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
            RtlAtomicAdd(&IoPageCacheMappedDirtyPageCount, (UINTN)-1);
        }
    }

    Status = STATUS_SUCCESS;

RemovePageCacheEntryVirtualAddressEnd:
    if (BackingEntry != NULL) {
        KeReleaseSharedExclusiveLockExclusive(BackingEntry->FileObject->Lock);
    }

    return Status;
}

VOID
IopRemovePageCacheEntryFromTree (
    PPAGE_CACHE_ENTRY PageCacheEntry
    )

/*++

Routine Description:

    This routine removes a page cache entry from the page cache tree. This
    routine assumes that the page cache's tree lock is held exclusively.

Arguments:

    PageCacheEntry - Supplies a pointer to the page cache entry to be removed.

Return Value:

    None.

--*/

{

    ASSERT(KeIsSharedExclusiveLockHeldExclusive(
                                            PageCacheEntry->FileObject->Lock));

    ASSERT(PageCacheEntry->Node.Parent != NULL);

    //
    // If a backing entry exists, then MM needs to know that the backing entry
    // now owns the page. It may have always been the owner, but just make sure.
    //

    if (PageCacheEntry->BackingEntry != NULL) {
        MmSetPageCacheEntryForPhysicalAddress(PageCacheEntry->PhysicalAddress,
                                              PageCacheEntry->BackingEntry);
    }

    RtlRedBlackTreeRemove(&(PageCacheEntry->FileObject->PageCacheTree),
                          &(PageCacheEntry->Node));

    PageCacheEntry->Node.Parent = NULL;
    IoPageCacheEntryCount -= 1;
    if ((IoPageCacheDebugFlags & PAGE_CACHE_DEBUG_EVICTION) != 0) {
        RtlDebugPrint("PAGE CACHE: Remove entry 0x%08x: file object "
                      "0x%08x, offset 0x%I64x, physical address "
                      "0x%I64x, reference count %d, flags 0x%08x.\n",
                      PageCacheEntry,
                      PageCacheEntry->FileObject,
                      PageCacheEntry->Offset,
                      PageCacheEntry->PhysicalAddress,
                      PageCacheEntry->ReferenceCount,
                      PageCacheEntry->Flags);
    }

    return;
}

VOID
IopUpdatePageCacheEntryList (
    PPAGE_CACHE_ENTRY PageCacheEntry,
    BOOL Created
    )

/*++

Routine Description:

    This routine updates a page cache entry's list entry by putting it on the
    appropriate list. This should be used when a page cache entry is looked up
    or when it is created.

Arguments:

    PageCacheEntry - Supplies a pointer to the page cache entry whose list
        entry needs to be updated.

    Created - Supplies a boolean indicating if the page cache entry was just
        created or not.

Return Value:

    None.

--*/

{

    KeAcquireQueuedLock(IoPageCacheListLock);

    //
    // If the page cache entry is not new, then it might already be on a
    // list. If it's on a clean list, move it to the back. If it's clean
    // and not on a list, then it probably got ripped off the list because
    // there are references on it.
    //

    if (Created == FALSE) {

        //
        // If it's dirty, it should always be on the dirty list.
        //

        ASSERT(((PageCacheEntry->Flags &
                 PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) ||
               (PageCacheEntry->ListEntry.Next != NULL));

        if (((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0) &&
            (PageCacheEntry->ListEntry.Next != NULL)) {

            LIST_REMOVE(&(PageCacheEntry->ListEntry));
            INSERT_BEFORE(&(PageCacheEntry->ListEntry),
                          &IoPageCacheCleanList);
        }

    //
    // New pages do not start on a list. Stick it on the back of the clean
    // list.
    //

    } else {

        ASSERT(PageCacheEntry->ListEntry.Next == NULL);
        ASSERT((PageCacheEntry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) == 0);

        INSERT_BEFORE(&(PageCacheEntry->ListEntry), &IoPageCacheCleanList);
    }

    KeReleaseQueuedLock(IoPageCacheListLock);
    return;
}

BOOL
IopIsPageCacheTooBig (
    PUINTN FreePhysicalPages
    )

/*++

Routine Description:

    This routine determines if the page cache is too large given current
    memory constraints.

Arguments:

    FreePhysicalPages - Supplies an optional pointer where the number of free
        physical pages used at the time of computation will be returned. This
        will only be returned if the page cache is reported to be too big.

Return Value:

    TRUE if the page cache is too big and should shrink.

    FALSE if the page cache is too small or just right.

--*/

{

    UINTN FreePages;

    //
    // Don't let the page cache shrink too much. If it's already below the
    // minimum just skip it (but leave the target remove count set so that
    // paging out is requested). Otherwise, clip the remove count to avoid
    // going below the minimum.
    //

    if (IoPageCachePhysicalPageCount <= IoPageCacheMinimumPages) {
        return FALSE;
    }

    //
    // Get the current number of free pages in the system, and determine if the
    // page cache still has room to grow.
    //

    FreePages = MmGetTotalFreePhysicalPages();
    if (FreePages > IoPageCacheHeadroomPagesTrigger) {
        return FALSE;
    }

    if (FreePhysicalPages != NULL) {
        *FreePhysicalPages = FreePages;
    }

    return TRUE;
}

BOOL
IopIsPageCacheTooMapped (
    PUINTN FreeVirtualPages
    )

/*++

Routine Description:

    This routine determines if the page cache has too many mapped entries given
    current memory constraints.

Arguments:

    FreeVirtualPages - Supplies an optional pointer where the number of free
        virtual pages at the time of computation will be returned. This will
        only be returned if the page cache is determined to have too many
        entries mapped.

Return Value:

    TRUE if the page cache has too many mapped entries and some should be
    unmapped.

    FALSE if the page cache does not have too many entries mapped.

--*/

{

    UINTN FreePages;

    //
    // Get the current number of free virtual pages in system memory and
    // determine if the page cache still has room to grow.
    //

    FreePages = MmGetFreeVirtualMemory() >> MmPageShift();
    if ((FreePages > IoPageCacheHeadroomVirtualPagesTrigger) &&
        (MmGetVirtualMemoryWarningLevel() == MemoryWarningLevelNone)) {

        return FALSE;
    }

    //
    // Check to make sure at least a single page cache entry is mapped.
    //

    if (IoPageCacheMappedPageCount == 0) {
        return FALSE;
    }

    if (FreeVirtualPages != NULL) {
        *FreeVirtualPages = FreePages;
    }

    return TRUE;
}

VOID
IopCheckFileObjectPageCache (
    PFILE_OBJECT FileObject
    )

/*++

Routine Description:

    This routine checks the given file object page cache for consistency.

Arguments:

    FileObject - Supplies a pointer to the file object to check.

Return Value:

    None.

--*/

{

    PLIST_ENTRY CurrentEntry;
    PPAGE_CACHE_ENTRY Entry;
    PRED_BLACK_TREE_NODE TreeNode;

    ASSERT(KeIsSharedExclusiveLockHeld(FileObject->Lock));

    KeAcquireQueuedLock(IoPageCacheListLock);
    TreeNode = RtlRedBlackTreeGetLowestNode(&(FileObject->PageCacheTree));
    while (TreeNode != NULL) {
        Entry = RED_BLACK_TREE_VALUE(TreeNode, PAGE_CACHE_ENTRY, Node);
        if ((Entry->Flags & PAGE_CACHE_ENTRY_FLAG_DIRTY) != 0) {
            if (Entry->ListEntry.Next == NULL) {
                RtlDebugPrint("PAGE_CACHE_ENTRY 0x%x for FILE_OBJECT 0x%x "
                              "Offset 0x%I64x dirty but not in list.\n",
                              Entry,
                              FileObject,
                              Entry->Offset);

            } else {
                CurrentEntry = FileObject->DirtyPageList.Next;
                while ((CurrentEntry != &(FileObject->DirtyPageList)) &&
                       (CurrentEntry != &(Entry->ListEntry))) {

                    CurrentEntry = CurrentEntry->Next;
                }

                if (CurrentEntry != &(Entry->ListEntry)) {
                    RtlDebugPrint("PAGE_CACHE_ENTRY 0x%x for FILE_OBJECT 0x%x "
                                  "Offset 0x%I64x dirty but not in dirty "
                                  "list.\n",
                                  Entry,
                                  FileObject,
                                  Entry->Offset);
                }
            }
        }

        TreeNode = RtlRedBlackTreeGetNextNode(&(FileObject->PageCacheTree),
                                              FALSE,
                                              TreeNode);
    }

    KeReleaseQueuedLock(IoPageCacheListLock);
    return;
}

