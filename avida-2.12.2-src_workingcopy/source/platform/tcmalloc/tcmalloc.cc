/*
 *  tcmalloc.cc
 *  Avida
 *
 *  Created by David on 3/20/06.
 *  Copyright 1999-2009 Michigan State University. All rights reserved.
 *
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; version 2
 *  of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

// Copyright (c) 2005, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// ---
// Author: Sanjay Ghemawat <opensource@google.com>
//
// A malloc that uses a per-thread cache to satisfy small malloc requests.
// (The time for malloc/free of a small object drops from 300 ns to 50 ns.)
//
// See doc/tcmalloc.html for a high-level
// description of how this malloc works.
//
// SYNCHRONIZATION
//  1. The thread-specific lists are accessed without acquiring any locks.
//     This is safe because each such list is only accessed by one thread.
//  2. We have a lock per central free-list, and hold it while manipulating
//     the central free list for a particular size.
//  3. The central page allocator is protected by "pageheap_lock".
//  4. The pagemap (which maps from page-number to descriptor),
//     can be read without holding any locks, and written while holding
//     the "pageheap_lock".
//
//     This multi-threaded access to the pagemap is safe for fairly
//     subtle reasons.  We basically assume that when an object X is
//     allocated by thread A and deallocated by thread B, there must
//     have been appropriate synchronization in the handoff of object
//     X from thread A to thread B.
//
// TODO: Bias reclamation to larger addresses
// TODO: implement mallinfo/mallopt
// TODO: Better testing
// TODO: Return memory to system
//
// 9/28/2003 (new page-level allocator replaces ptmalloc2):
// * malloc/free of small objects goes from ~300 ns to ~50 ns.
// * allocation of a reasonably complicated struct
//   goes from about 1100 ns to about 300 ns.

#include "tcmalloc-config.h"

#include <new>
#include <cstdio>
#include <stddef.h>
#include <stdint.h>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#include <cstdarg>

#include "tcmalloc-logging.h"
#include "spinlock.h"
#include "pagemap.h"
#include "system-alloc.h"

#if defined HAVE_INTTYPES_H
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#define LLU   PRIu64
#else
#define LLU   "llu"              // hope for the best
#endif

//-------------------------------------------------------------------
// Configuration
//-------------------------------------------------------------------

// Not all possible combinations of the following parameters make
// sense.  In particular, if kMaxSize increases, you may have to
// increase kNumClasses as well.
static const size_t kPageShift  = 12;
static const size_t kPageSize   = 1 << kPageShift;
static const size_t kMaxSize    = 8u * kPageSize;
static const size_t kAlignShift = 3;
static const size_t kAlignment  = 1 << kAlignShift;
static const size_t kNumClasses = 170;

// Allocates a big block of memory for the pagemap once we reach more than
// 128MB
static const size_t kPageMapBigAllocationThreshold = 128 << 20;

// Minimum number of pages to fetch from system at a time.  Must be
// significantly bigger than kBlockSize to amortize system-call
// overhead, and also to reduce external fragementation.  Also, we
// should keep this value big because various incarnations of Linux
// have small limits on the number of mmap() regions per
// address-space.
static const unsigned int kMinSystemAlloc = 1 << (20 - kPageShift);

// Number of objects to move between a per-thread list and a central
// list in one shot.  We want this to be not too small so we can
// amortize the lock overhead for accessing the central list.  Making
// it too big may temporarily cause unnecessary memory wastage in the
// per-thread free list until the scavenger cleans up the list.
static int num_objects_to_move[kNumClasses];

// Maximum length we allow a per-thread free-list to have before we
// move objects from it into the corresponding central free-list.  We
// want this big to avoid locking the central free-list too often.  It
// should not hurt to make this list somewhat big because the
// scavenging code will shrink it down when its contents are not in use.
static const int kMaxFreeListLength = 256;

// Lower and upper bounds on the per-thread cache sizes
static const size_t kMinThreadCacheSize = kMaxSize * 2;
static const size_t kMaxThreadCacheSize = 2 << 20;

// Default bound on the total amount of thread caches
static const size_t kDefaultOverallThreadCacheSize = 16 << 20;

// For all span-lengths < kMaxPages we keep an exact-size list.
// REQUIRED: kMaxPages >= kMinSystemAlloc;
static const size_t kMaxPages = kMinSystemAlloc;

// Twice the approximate gap between sampling actions.
// I.e., we take one sample approximately once every
//      kSampleParameter/2
// bytes of allocation, i.e., ~ once every 128KB.
// Must be a prime number.
static const size_t kSampleParameter = 266053;

//-------------------------------------------------------------------
// Mapping from size to size_class and vice versa
//-------------------------------------------------------------------

// A pair of arrays we use for implementing the mapping from a size to
// its size class.  Indexed by "floor(lg(size))".
static const int kSizeBits = 8 * sizeof(size_t);
static unsigned char size_base[kSizeBits];
static unsigned char size_shift[kSizeBits];

// Mapping from size class to size
static size_t class_to_size[kNumClasses];

// Mapping from size class to number of pages to allocate at a time
static size_t class_to_pages[kNumClasses];



// TransferCache is used to cache transfers of num_objects_to_move[size_class]
// back and forth between thread caches and the central cache for a given size
// class.
struct TCEntry {
  void *head;  // Head of chain of objects.
  void *tail;  // Tail of chain of objects.
};
// A central cache freelist can have anywhere from 0 to kNumTransferEntries
// slots to put link list chains into.  To keep memory usage bounded the total
// number of TCEntries across size classes is fixed.  Currently each size
// class is initially given one TCEntry which also means that the maximum any
// one class can have is kNumClasses.
static const int kNumTransferEntries = kNumClasses;

// Return floor(log2(n)) for n > 0.
#if PLATFORM(X86) && COMPILER(GCC)
static inline int LgFloor(size_t n) {
  // "ro" for the input spec means the input can come from either a
  // register ("r") or offsetable memory ("o").
  int result;
  __asm__("bsrl  %1, %0"
          : "=r" (result)               // Output spec
          : "ro" (n)                    // Input spec
          : "cc"                        // Clobbers condition-codes
          );
  return result;
}

#elif PLATFORM(PPC) && COMPILER(GCC)
static inline int LgFloor(size_t n) {
  // "r" for the input spec means the input must come from a
  // register ("r")
  int result;
  
  __asm__ ("{cntlz|cntlzw} %0,%1" 
           : "=r" (result)              // Output spec
           : "r" (n));                  // Input spec
  
  return 31 - result;
}

#else
// Note: the following only works for "n"s that fit in 32-bits, but
// that is fine since we only use it for small sizes.
static inline int LgFloor(size_t n) {
  int log = 0;
  for (int i = 4; i >= 0; --i) {
    int shift = (1 << i);
    size_t x = n >> shift;
    if (x != 0) {
      n = x;
      log += shift;
    }
  }
  ASSERT(n == 1);
  return log;
}
#endif


// Some very basic linked list functions for dealing with using void * as
// storage.

static inline void *SLL_Next(void *t) {
  return *(reinterpret_cast<void**>(t));
}

static inline void SLL_SetNext(void *t, void *n) {
  *(reinterpret_cast<void**>(t)) = n;
}

static inline void SLL_Push(void **list, void *element) {
  SLL_SetNext(element, *list);
  *list = element;
}

static inline void *SLL_Pop(void **list) {
  void *result = *list;
  *list = SLL_Next(*list);
  return result;
}


// Remove N elements from a linked list to which head points.  head will be
// modified to point to the new head.  start and end will point to the first
// and last nodes of the range.  Note that end will point to NULL after this
// function is called.
static inline void SLL_PopRange(void **head, int N, void **start, void **end) {
  if (N == 0) {
    *start = NULL;
    *end = NULL;
    return;
  }

  void *tmp = *head;
  for (int i = 1; i < N; ++i) {
    tmp = SLL_Next(tmp);
  }

  *start = *head;
  *end = tmp;
  *head = SLL_Next(tmp);
  // Unlink range from list.
  SLL_SetNext(tmp, NULL);
}

static inline void SLL_PushRange(void **head, void *start, void *end) {
  if (!start) return;
  SLL_SetNext(end, *head);
  *head = start;
}

static inline size_t SLL_Size(void *head) {
  int count = 0;
  while (head) {
    count++;
    head = SLL_Next(head);
  }
  return count;
}

// Setup helper functions.

static inline int SizeClass(size_t size) {
  if (size == 0) size = 1;
  const int lg = LgFloor(size);
  const int align = size_shift[lg];
  return static_cast<int>(size_base[lg]) + ((size-1) >> align);
}

// Get the byte-size for a specified class
static inline size_t ByteSizeForClass(size_t cl) {
  return class_to_size[cl];
}


static int NumMoveSize(size_t size) {
  if (size == 0) return 0;
  // Use approx 64k transfers between thread and central caches.
  int num = static_cast<int>(64.0 * 1024.0 / size);
  if (num < 2) num = 2;
  // Clamp well below kMaxFreeListLength to avoid ping pong between central
  // and thread caches.
  if (num > static_cast<int>(0.8 * kMaxFreeListLength))
    num = static_cast<int>(0.8 * kMaxFreeListLength);
  return num;
}

// Initialize the mapping arrays
static void InitSizeClasses() {
  // Special initialization for small sizes
  for (unsigned int lg = 0; lg < kAlignShift; lg++) {
    size_base[lg] = 1;
    size_shift[lg] = kAlignShift;
  }

  unsigned int next_class = 1;
  int alignshift = kAlignShift;
  int last_lg = -1;
  for (size_t size = kAlignment; size <= kMaxSize; size += (1 << alignshift)) {
    int lg = LgFloor(size);
    if (lg > last_lg) {
      // Increase alignment every so often.
      //
      // Since we double the alignment every time size doubles and
      // size >= 128, this means that space wasted due to alignment is
      // at most 16/128 i.e., 12.5%.  Plus we cap the alignment at 256
      // bytes, so the space wasted as a percentage starts falling for
      // sizes > 2K.
      if ((lg >= 7) && (alignshift < 8)) {
        alignshift++;
      }
      size_base[lg] = next_class - ((size-1) >> alignshift);
      size_shift[lg] = alignshift;
    }

    class_to_size[next_class] = size;
    last_lg = lg;

    next_class++;
  }
  if (next_class >= kNumClasses) {
    MESSAGE("used up too many size classes: %d\n", next_class);
    abort();
  }

  // Initialize the number of pages we should allocate to split into
  // small objects for a given class.
  for (size_t cl = 1; cl < next_class; cl++) {
    // Allocate enough pages so leftover is less than 1/8 of total.
    // This bounds wasted space to at most 12.5%.
    size_t psize = kPageSize;
    const size_t s = class_to_size[cl];
    while ((psize % s) > (psize >> 3)) {
      psize += kPageSize;
    }
    class_to_pages[cl] = psize >> kPageShift;
  }

  // Double-check sizes just to be safe
  for (size_t size = 0; size <= kMaxSize; size++) {
    const unsigned int sc = SizeClass(size);
    if (sc == 0) {
      MESSAGE("Bad size class %d for %" PRIuS "\n", sc, size);
      abort();
    }
    if (sc > 1 && size <= class_to_size[sc-1]) {
      MESSAGE("Allocating unnecessarily large class %d for %" PRIuS
              "\n", sc, size);
      abort();
    }
    if (sc >= kNumClasses) {
      MESSAGE("Bad size class %d for %" PRIuS "\n", sc, size);
      abort();
    }
    const size_t s = class_to_size[sc];
    if (size > s) {
      MESSAGE("Bad size %" PRIuS " for %" PRIuS " (sc = %d)\n", s, size, sc);
      abort();
    }
    if (s == 0) {
      MESSAGE("Bad size %" PRIuS " for %" PRIuS " (sc = %d)\n", s, size, sc);
      abort();
    }
  }

  // Initialize the num_objects_to_move array.
  for (size_t cl = 1; cl  < kNumClasses; ++cl) {
    num_objects_to_move[cl] = NumMoveSize(ByteSizeForClass(cl));
  }
}

// -------------------------------------------------------------------------
// Simple allocator for objects of a specified type.  External locking
// is required before accessing one of these objects.
// -------------------------------------------------------------------------

// Metadata allocator -- keeps stats about how many bytes allocated
static uint64_t metadata_system_bytes = 0;
static void* MetaDataAlloc(size_t bytes) {
  void* result = TCMalloc_SystemAlloc(bytes);
  if (result != NULL) {
    metadata_system_bytes += bytes;
  }
  return result;
}

template <class T>
class PageHeapAllocator {
 private:
  // How much to allocate from system at a time
  static const unsigned int kAllocIncrement = 128 << 10;

  // Aligned size of T
  static const size_t kAlignedSize
  = (((sizeof(T) + kAlignment - 1) / kAlignment) * kAlignment);

  // Free area from which to carve new objects
  char* free_area_;
  size_t free_avail_;

  // Free list of already carved objects
  void* free_list_;

  // Number of allocated but unfreed objects
  int inuse_;

 public:
  void Init() {
    ASSERT(kAlignedSize <= kAllocIncrement);
    inuse_ = 0;
    free_area_ = NULL;
    free_avail_ = 0;
    free_list_ = NULL;
    // Reserve some space at the beginning to avoid fragmentation.
    Delete(New());
  }

  T* New() {
    // Consult free list
    void* result;
    if (free_list_ != NULL) {
      result = free_list_;
      free_list_ = *(reinterpret_cast<void**>(result));
    } else {
      if (free_avail_ < kAlignedSize) {
        // Need more room
        free_area_ = reinterpret_cast<char*>(MetaDataAlloc(kAllocIncrement));
        if (free_area_ == NULL) abort();
        free_avail_ = kAllocIncrement;
      }
      result = free_area_;
      free_area_ += kAlignedSize;
      free_avail_ -= kAlignedSize;
    }
    inuse_++;
    return reinterpret_cast<T*>(result);
  }

  void Delete(T* p) {
    *(reinterpret_cast<void**>(p)) = free_list_;
    free_list_ = p;
    inuse_--;
  }

  int inuse() const { return inuse_; }
};

// -------------------------------------------------------------------------
// Span - a contiguous run of pages
// -------------------------------------------------------------------------

// Type that can hold a page number
typedef uintptr_t PageID;

// Type that can hold the length of a run of pages
typedef uintptr_t Length;

// Convert byte size into pages
static inline Length pages(size_t bytes) {
  return ((bytes + kPageSize - 1) >> kPageShift);
}

// Convert a user size into the number of bytes that will actually be
// allocated
static size_t AllocationSize(size_t bytes) {
  if (bytes > kMaxSize) {
    // Large object: we allocate an integral number of pages
    return pages(bytes) << kPageShift;
  } else {
    // Small object: find the size class to which it belongs
    return ByteSizeForClass(SizeClass(bytes));
  }
}

// Information kept for a span (a contiguous run of pages).
struct Span {
  PageID        start;          // Starting page number
  Length        length;         // Number of pages in span
  Span*         next;           // Used when in link list
  Span*         prev;           // Used when in link list
  void*         objects;        // Linked list of free objects
  unsigned int  free : 1;       // Is the span free
  unsigned int  sample : 1;     // Sampled object?
  unsigned int  sizeclass : 8;  // Size-class for small objects (or 0)
  unsigned int  refcount : 11;  // Number of non-free objects

#undef SPAN_HISTORY
#ifdef SPAN_HISTORY
  // For debugging, we can keep a log events per span
  int nexthistory;
  char history[64];
  int value[64];
#endif
};

#ifdef SPAN_HISTORY
void Event(Span* span, char op, int v = 0) {
  span->history[span->nexthistory] = op;
  span->value[span->nexthistory] = v;
  span->nexthistory++;
  if (span->nexthistory == sizeof(span->history)) span->nexthistory = 0;
}
#else
#define Event(s,o,v) ((void) 0)
#endif

// Allocator/deallocator for spans
static PageHeapAllocator<Span> span_allocator;
static Span* NewSpan(PageID p, Length len) {
  Span* result = span_allocator.New();
  memset(result, 0, sizeof(*result));
  result->start = p;
  result->length = len;
#ifdef SPAN_HISTORY
  result->nexthistory = 0;
#endif
  return result;
}

static inline void DeleteSpan(Span* span) {
#ifndef NDEBUG
  // In debug mode, trash the contents of deleted Spans
  memset(span, 0x3f, sizeof(*span));
#endif
  span_allocator.Delete(span);
}

// -------------------------------------------------------------------------
// Doubly linked list of spans.
// -------------------------------------------------------------------------

static inline void DLL_Init(Span* list) {
  list->next = list;
  list->prev = list;
}

static inline void DLL_Remove(Span* span) {
  span->prev->next = span->next;
  span->next->prev = span->prev;
  span->prev = NULL;
  span->next = NULL;
}

static inline bool DLL_IsEmpty(const Span* list) {
  return list->next == list;
}

//static int DLL_Length(const Span* list) {
//  int result = 0;
//  for (Span* s = list->next; s != list; s = s->next) {
//    result++;
//  }
//  return result;
//}

#if 0 /* Not needed at the moment -- causes compiler warnings if not used */
static void DLL_Print(const char* label, const Span* list) {
  MESSAGE("%-10s %p:", label, list);
  for (const Span* s = list->next; s != list; s = s->next) {
    MESSAGE(" <%p,%u,%u>", s, s->start, s->length);
  }
  MESSAGE("\n");
}
#endif

static inline void DLL_Prepend(Span* list, Span* span) {
  ASSERT(span->next == NULL);
  ASSERT(span->prev == NULL);
  span->next = list->next;
  span->prev = list;
  list->next->prev = span;
  list->next = span;
}

static void DLL_InsertOrdered(Span* list, Span* span) {
  ASSERT(span->next == NULL);
  ASSERT(span->prev == NULL);
  // Look for appropriate place to insert
  Span* x = list;
  while ((x->next != list) && (x->next->start < span->start)) {
    x = x->next;
  }
  span->next = x->next;
  span->prev = x;
  x->next->prev = span;
  x->next = span;
}

// -------------------------------------------------------------------------
// Stack traces kept for sampled allocations
//   The following state is protected by pageheap_lock_.
// -------------------------------------------------------------------------

static const int kMaxStackDepth = 31;
struct StackTrace {
  uintptr_t size;          // Size of object
  int       depth;         // Number of PC values stored in array below
  void*     stack[kMaxStackDepth];
};
static PageHeapAllocator<StackTrace> stacktrace_allocator;
static Span sampled_objects;

// Linked list of stack traces recorded every time we allocated memory
// from the system.  Useful for finding allocation sites that cause
// increase in the footprint of the system.  The linked list pointer
// is stored in trace->stack[kMaxStackDepth-1].
// @not_used -static StackTrace* growth_stacks = NULL;

// -------------------------------------------------------------------------
// Map from page-id to per-page data
// -------------------------------------------------------------------------

// We use PageMap2<> for 32-bit and PageMap3<> for 64-bit machines.

// Selector class -- general selector uses 3-level map
template <int BITS> class MapSelector {
 public:
  typedef TCMalloc_PageMap3<BITS-kPageShift> Type;
};

// A two-level map for 32-bit machines
template <> class MapSelector<32> {
 public:
  typedef TCMalloc_PageMap2<32-kPageShift> Type;
};

// -------------------------------------------------------------------------
// Page-level allocator
//  * Eager coalescing
//
// Heap for page-level allocation.  We allow allocating and freeing a
// contiguous runs of pages (called a "span").
// -------------------------------------------------------------------------

class TCMalloc_PageHeap {
 public:
  TCMalloc_PageHeap();

  // Allocate a run of "n" pages.  Returns zero if out of memory.
  // Caller should not pass "n == 0" -- instead, n should have
  // been rounded up already.
  Span* New(Length n);

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() and
  //           has not yet been deleted.
  void Delete(Span* span);

  // Mark an allocated span as being used for small objects of the
  // specified size-class.
  // REQUIRES: span was returned by an earlier call to New()
  //           and has not yet been deleted.
  void RegisterSizeClass(Span* span, size_t sc);

  // Split an allocated span into two spans: one of length "n" pages
  // followed by another span of length "span->length - n" pages.
  // Modifies "*span" to point to the first span of length "n" pages.
  // Returns a pointer to the second span.
  //
  // REQUIRES: "0 < n < span->length"
  // REQUIRES: !span->free
  // REQUIRES: span->sizeclass == 0
  Span* Split(Span* span, Length n);

  // Return the descriptor for the specified page.
  inline Span* GetDescriptor(PageID p) const {
    return reinterpret_cast<Span*>(pagemap_.get(p));
  }

// @not_used -  // Dump state to stderr
// @not_used -  void Dump(TCMalloc_Printer* out);

  // Return number of bytes allocated from system
  inline uint64_t SystemBytes() const { return system_bytes_; }

  // Return number of free bytes in heap
  uint64_t FreeBytes() const {
    return (static_cast<uint64_t>(free_pages_) << kPageShift);
  }

  bool Check();
  bool CheckList(Span* list, Length min_pages, Length max_pages);

 private:
  // Pick the appropriate map type based on pointer size
  typedef MapSelector<8*sizeof(uintptr_t)>::Type PageMap;
  PageMap pagemap_;

  // List of free spans of length >= kMaxPages
  Span large_;

  // Array mapping from span length to a doubly linked list of free spans
  Span free_[kMaxPages];

  // Number of pages kept in free lists
  uintptr_t free_pages_;

  // Bytes allocated from system
  uint64_t system_bytes_;

  bool GrowHeap(Length n);

  // REQUIRES   span->length >= n
  // Remove span from its free list, and move any leftover part of
  // span into appropriate free lists.  Also update "span" to have
  // length exactly "n" and mark it as non-free so it can be returned
  // to the client.
  void Carve(Span* span, Length n);

  void RecordSpan(Span* span) {
    pagemap_.set(span->start, span);
    if (span->length > 1) {
      pagemap_.set(span->start + span->length - 1, span);
    }
  }
};

TCMalloc_PageHeap::TCMalloc_PageHeap() : pagemap_(MetaDataAlloc),
                                         free_pages_(0),
                                         system_bytes_(0) {
  DLL_Init(&large_);
  for (unsigned int i = 0; i < kMaxPages; i++) {
    DLL_Init(&free_[i]);
  }
}

inline Span* TCMalloc_PageHeap::New(Length n) {
  ASSERT(Check());

  // n==0 occurs iff pages() overflowed when we added kPageSize-1 to n
  if (n == 0) return NULL;

  // Find first size >= n that has a non-empty list
  for (Length s = n; s < kMaxPages; s++) {
    if (!DLL_IsEmpty(&free_[s])) {
      Span* result = free_[s].next;
      Carve(result, n);
      ASSERT(Check());
      free_pages_ -= n;
      return result;
    }
  }

  // Look in large list.  If we first do not find something, we try to
  // grow the heap and try again.
  for (int i = 0; i < 2; i++) {
    // find the best span (closest to n in size)
    Span *best = NULL;
    for (Span* span = large_.next; span != &large_; span = span->next) {
      if (span->length >= n &&
          (best == NULL || span->length < best->length)) {
        best = span;
      }
    }
    if (best != NULL) {
      Carve(best, n);
      ASSERT(Check());
      free_pages_ -= n;
      return best;
    }
    if (i == 0) {
      // Nothing suitable in large list.  Grow the heap and look again.
      if (!GrowHeap(n)) {
        ASSERT(Check());
        return NULL;
      }
    }
  }
  return NULL;
}

Span* TCMalloc_PageHeap::Split(Span* span, Length n) {
  ASSERT(0 < n);
  ASSERT(n < span->length);
  ASSERT(!span->free);
  ASSERT(span->sizeclass == 0);
  Event(span, 'T', n);

  const int extra = span->length - n;
  Span* leftover = NewSpan(span->start + n, extra);
  Event(leftover, 'U', extra);
  RecordSpan(leftover);
  pagemap_.set(span->start + n - 1, span); // Update map from pageid to span
  span->length = n;

  return leftover;
}

inline void TCMalloc_PageHeap::Carve(Span* span, Length n) {
  ASSERT(n > 0);
  DLL_Remove(span);
  span->free = 0;
  Event(span, 'A', n);

  ASSERT(((int)span->length - 1) >= 0);
  const unsigned int extra = span->length - n;
  if (extra > 0) {
    Span* leftover = NewSpan(span->start + n, extra);
    leftover->free = 1;
    Event(leftover, 'S', extra);
    RecordSpan(leftover);
    if (extra < kMaxPages) {
      DLL_Prepend(&free_[extra], leftover);
    } else {
      DLL_InsertOrdered(&large_, leftover);
    }
    span->length = n;
    pagemap_.set(span->start + n - 1, span);
  }
}

inline void TCMalloc_PageHeap::Delete(Span* span) {
  ASSERT(Check());
  ASSERT(!span->free);
  ASSERT(span->length > 0);
  ASSERT(GetDescriptor(span->start) == span);
  ASSERT(GetDescriptor(span->start + span->length - 1) == span);
  span->sizeclass = 0;
  span->sample = 0;

  // Coalesce -- we guarantee that "p" != 0, so no bounds checking
  // necessary.  We do not bother resetting the stale pagemap
  // entries for the pieces we are merging together because we only
  // care about the pagemap entries for the boundaries.
  const PageID p = span->start;
  const Length n = span->length;
  Span* prev = GetDescriptor(p-1);
  if (prev != NULL && prev->free) {
    // Merge preceding span into this span
    ASSERT(prev->start + prev->length == p);
    const Length len = prev->length;
    DLL_Remove(prev);
    DeleteSpan(prev);
    span->start -= len;
    span->length += len;
    pagemap_.set(span->start, span);
    Event(span, 'L', len);
  }
  Span* next = GetDescriptor(p+n);
  if (next != NULL && next->free) {
    // Merge next span into this span
    ASSERT(next->start == p+n);
    const Length len = next->length;
    DLL_Remove(next);
    DeleteSpan(next);
    span->length += len;
    pagemap_.set(span->start + span->length - 1, span);
    Event(span, 'R', len);
  }

  Event(span, 'D', span->length);
  span->free = 1;
  if (span->length < kMaxPages) {
    DLL_Prepend(&free_[span->length], span);
  } else {
    DLL_InsertOrdered(&large_, span);
  }
  free_pages_ += n;

  ASSERT(Check());
}

void TCMalloc_PageHeap::RegisterSizeClass(Span* span, size_t sc) {
  // Associate span object with all interior pages as well
  ASSERT(!span->free);
  ASSERT(GetDescriptor(span->start) == span);
  ASSERT(GetDescriptor(span->start+span->length-1) == span);
  Event(span, 'C', sc);
  span->sizeclass = sc;
  for (Length i = 1; i < span->length-1; i++) {
    pagemap_.set(span->start+i, span);
  }
}

// @not_used -void TCMalloc_PageHeap::Dump(TCMalloc_Printer* out) {
// @not_used -  int nonempty_sizes = 0;
// @not_used -  for (int s = 0; s < kMaxPages; s++) {
// @not_used -    if (!DLL_IsEmpty(&free_[s])) nonempty_sizes++;
// @not_used -  }
// @not_used -  out->printf("------------------------------------------------\n");
// @not_used -  out->printf("PageHeap: %d sizes; %6.1f MB free\n", nonempty_sizes,
// @not_used -              (static_cast<double>(free_pages_) * kPageSize) / 1048576.0);
// @not_used -  out->printf("------------------------------------------------\n");
// @not_used -  uint64_t cumulative = 0;
// @not_used -  for (int s = 0; s < kMaxPages; s++) {
// @not_used -    if (!DLL_IsEmpty(&free_[s])) {
// @not_used -      const int list_length = DLL_Length(&free_[s]);
// @not_used -      uint64_t s_pages = s * list_length;
// @not_used -      cumulative += s_pages;
// @not_used -      out->printf("%6u pages * %6u spans ~ %6.1f MB; %6.1f MB cum\n",
// @not_used -                  s, list_length,
// @not_used -                  (s_pages << kPageShift) / 1048576.0,
// @not_used -                  (cumulative << kPageShift) / 1048576.0);
// @not_used -    }
// @not_used -  }
// @not_used -
// @not_used -  uint64_t large_pages = 0;
// @not_used -  int large_spans = 0;
// @not_used -  for (Span* s = large_.next; s != &large_; s = s->next) {
// @not_used -    out->printf("   [ %6" PRIuS " spans ]\n", s->length);
// @not_used -    large_pages += s->length;
// @not_used -    large_spans++;
// @not_used -  }
// @not_used -  cumulative += large_pages;
// @not_used -  out->printf(">255   large * %6u spans ~ %6.1f MB; %6.1f MB cum\n",
// @not_used -              large_spans,
// @not_used -              (large_pages << kPageShift) / 1048576.0,
// @not_used -              (cumulative << kPageShift) / 1048576.0);
// @not_used -}

// @not_used -static void RecordGrowth(size_t growth) {
// @not_used -  StackTrace* t = stacktrace_allocator.New();
// @not_used -  t->depth = GetStackTrace(t->stack, kMaxStackDepth-1, 3);
// @not_used -  t->size = growth;
// @not_used -  t->stack[kMaxStackDepth-1] = reinterpret_cast<void*>(growth_stacks);
// @not_used -  growth_stacks = t;
// @not_used -}

bool TCMalloc_PageHeap::GrowHeap(Length n) {
  ASSERT(kMaxPages >= kMinSystemAlloc);
  Length ask = (n>kMinSystemAlloc) ? n : static_cast<Length>(kMinSystemAlloc);
  void* ptr = TCMalloc_SystemAlloc(ask << kPageShift, kPageSize);
  if (ptr == NULL) {
    if (n < ask) {
      // Try growing just "n" pages
      ask = n;
      ptr = TCMalloc_SystemAlloc(ask << kPageShift, kPageSize);
    }
    if (ptr == NULL) return false;
  }
// @not_used -  RecordGrowth(ask << kPageShift);

  uint64_t old_system_bytes = system_bytes_;
  system_bytes_ += (ask << kPageShift);
  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  ASSERT(p > 0);

  // If we have already a lot of pages allocated, just pre allocate a bunch of
  // memory for the page map. This prevents fragmentation by pagemap metadata
  // when a program keeps allocating and freeing large blocks.

  if (old_system_bytes < kPageMapBigAllocationThreshold
      && system_bytes_ >= kPageMapBigAllocationThreshold) {
    pagemap_.PreallocateMoreMemory();
  }

  // Make sure pagemap_ has entries for all of the new pages.
  // Plus ensure one before and one after so coalescing code
  // does not need bounds-checking.
  if (pagemap_.Ensure(p-1, ask+2)) {
    // Pretend the new area is allocated and then Delete() it to
    // cause any necessary coalescing to occur.
    //
    // We do not adjust free_pages_ here since Delete() will do it for us.
    Span* span = NewSpan(p, ask);
    RecordSpan(span);
    Delete(span);
    ASSERT(Check());
    return true;
  } else {
    // We could not allocate memory within "pagemap_"
    // TODO: Once we can return memory to the system, return the new span
    return false;
  }
}

bool TCMalloc_PageHeap::Check() {
  ASSERT(free_[0].next == &free_[0]);
  CheckList(&large_, kMaxPages, 1000000000);
  for (Length s = 1; s < kMaxPages; s++) {
    CheckList(&free_[s], s, s);
  }
  return true;
}

bool TCMalloc_PageHeap::CheckList(Span* list, Length min_pages, Length max_pages) {
  for (Span* s = list->next; s != list; s = s->next) {
    CHECK_CONDITION(s->free);
    CHECK_CONDITION(s->length >= min_pages);
    CHECK_CONDITION(s->length <= max_pages);
    CHECK_CONDITION(GetDescriptor(s->start) == s);
    CHECK_CONDITION(GetDescriptor(s->start+s->length-1) == s);
  }
  return true;
}

//-------------------------------------------------------------------
// Free list
//-------------------------------------------------------------------

class TCMalloc_ThreadCache_FreeList {
 private:
  void*    list_;       // Linked list of nodes
  uint16_t length_;     // Current length
  uint16_t lowater_;    // Low water mark for list length

 public:
  void Init() {
    list_ = NULL;
    length_ = 0;
    lowater_ = 0;
  }

  // Return current length of list
  int length() const {
    return length_;
  }

  // Is list empty?
  bool empty() const {
    return list_ == NULL;
  }

  // Low-water mark management
  int lowwatermark() const { return lowater_; }
  void clear_lowwatermark() { lowater_ = length_; }

  inline void Push(void* ptr) {
    SLL_Push(&list_, ptr);
    length_++;
  }

  inline void* Pop() {
    ASSERT(list_ != NULL);
    length_--;
    if (length_ < lowater_) lowater_ = length_;
    return SLL_Pop(&list_);
  }

  inline void PushRange(int N, void *start, void *end) {
    SLL_PushRange(&list_, start, end);
    length_ += N;
  }

  inline void PopRange(int N, void **start, void **end) {
    SLL_PopRange(&list_, N, start, end);
    ASSERT(length_ >= N);
    length_ -= N;
  }
};

//-------------------------------------------------------------------
// Data kept per thread
//-------------------------------------------------------------------

class TCMalloc_ThreadCache {
 private:
  typedef TCMalloc_ThreadCache_FreeList FreeList;

  size_t        size_;                  // Combined size of data
  pthread_t     tid_;                   // Which thread owns it
  bool          in_setspecific_;        // In call to pthread_setspecific?
  FreeList      list_[kNumClasses];     // Array indexed by size-class

  // We sample allocations, biased by the size of the allocation
  uint32_t      rnd_;                   // Cheap random number generator
  size_t        bytes_until_sample_;    // Bytes until we sample next

 public:
  // All ThreadCache objects are kept in a linked list (for stats collection)
  TCMalloc_ThreadCache* next_;
  TCMalloc_ThreadCache* prev_;

  void Init(pthread_t tid);
  void Cleanup();

  // Accessors (mostly just for printing stats)
  int freelist_length(size_t cl) const { return list_[cl].length(); }

  // Total byte size in cache
  size_t Size() const { return size_; }

  void* Allocate(size_t size);
  void Deallocate(void* ptr, size_t size_class);

  void FetchFromCentralCache(size_t cl);
  void ReleaseToCentralCache(size_t cl, int N);
  void Scavenge();
  void Print() const;

  // Record allocation of "k" bytes.  Return true iff allocation
  // should be sampled
  bool SampleAllocation(size_t k);

  // Pick next sampling point
  void PickNextSample();

  static void                  InitModule();
  static void                  InitTSD();
  static TCMalloc_ThreadCache* GetCache();
  static TCMalloc_ThreadCache* GetCacheIfPresent();
  static void*                 CreateCacheIfNecessary();
  static void                  DeleteCache(void* ptr);
  static void                  RecomputeThreadCacheSize();
};

//-------------------------------------------------------------------
// Data kept per size-class in central cache
//-------------------------------------------------------------------

class TCMalloc_Central_FreeList {
 public:
  void Init(size_t cl);

  // These methods all do internal locking.

  // Insert the specified range into the central freelist.  N is the number of
  // elements in the range.
  void InsertRange(void *start, void *end, int N);

  // Returns the actual number of fetched elements into N.
  void RemoveRange(void **start, void **end, int *N);

  // Returns the number of free objects in cache.
  int length() {
    SpinLockHolder h(&lock_);
    return counter_;
  }

  // Returns the number of free objects in the transfer cache.
  int tc_length() {
    SpinLockHolder h(&lock_);
    return used_slots_ * num_objects_to_move[size_class_];
  }

 private:
  // REQUIRES: lock_ is held
  // Remove object from cache and return.
  // Return NULL if no free entries in cache.
  void* FetchFromSpans();

  // REQUIRES: lock_ is held
  // Remove object from cache and return.  Fetches
  // from pageheap if cache is empty.  Only returns
  // NULL on allocation failure.
  void* FetchFromSpansSafe();

  // REQUIRES: lock_ is held
  // Release a linked list of objects to spans.
  // May temporarily release lock_.
  void ReleaseListToSpans(void *start);

  // REQUIRES: lock_ is held
  // Release an object to spans.
  // May temporarily release lock_.
  void ReleaseToSpans(void* object);

  // REQUIRES: lock_ is held
  // Populate cache by fetching from the page heap.
  // May temporarily release lock_.
  void Populate();

  // REQUIRES: lock is held.
  // Tries to make room for a TCEntry.  If the cache is full it will try to
  // expand it at the cost of some other cache size.  Return false if there is
  // no space.
  bool MakeCacheSpace();

  // REQUIRES: lock_ for locked_size_class is held.
  // Picks a "random" size class to steal TCEntry slot from.  In reality it
  // just iterates over the sizeclasses but does so without taking a lock.
  // Returns true on success.
  // May temporarily lock a "random" size class.
  static bool EvictRandomSizeClass(unsigned int locked_size_class, bool force);

  // REQUIRES: lock_ is *not* held.
  // Tries to shrink the Cache.  If force is true it will relase objects to
  // spans if it allows it to shrink the cache.  Return false if it failed to
  // shrink the cache.  Decrements cache_size_ on succeess.
  // May temporarily take lock_.  If it takes lock_, the locked_size_class
  // lock is released to the thread from holding two size class locks
  // concurrently which could lead to a deadlock.
  bool ShrinkCache(int locked_size_class, bool force);

  // This lock protects all the data members.  cached_entries and cache_size_
  // may be looked at without holding the lock.
  SpinLock lock_;

  // We keep linked lists of empty and non-empty spans.
  size_t   size_class_;     // My size class
  Span     empty_;          // Dummy header for list of empty spans
  Span     nonempty_;       // Dummy header for list of non-empty spans
  size_t   counter_;        // Number of free objects in cache entry

  // Here we reserve space for TCEntry cache slots.  Since one size class can
  // end up getting all the TCEntries quota in the system we just preallocate
  // sufficient number of entries here.
  TCEntry tc_slots_[kNumTransferEntries];

  // Number of currently used cached entries in tc_slots_.  This variable is
  // updated under a lock but can be read without one.
  int32_t used_slots_;
  // The current number of slots for this size class.  This is an
  // adaptive value that is increased if there is lots of traffic
  // on a given size class.
  int32_t cache_size_;
};

// Pad each CentralCache object to multiple of 64 bytes
class TCMalloc_Central_FreeListPadded : public TCMalloc_Central_FreeList {
 private:
  char pad_[(64 - (sizeof(TCMalloc_Central_FreeList) % 64)) % 64];
};

//-------------------------------------------------------------------
// Global variables
//-------------------------------------------------------------------

// Central cache -- a collection of free-lists, one per size-class.
// We have a separate lock per free-list to reduce contention.
static TCMalloc_Central_FreeListPadded central_cache[kNumClasses];

// Page-level allocator
static SpinLock pageheap_lock = SPINLOCK_INITIALIZER;
static char pageheap_memory[sizeof(TCMalloc_PageHeap)];
static bool phinited = false;

// Avoid extra level of indirection by making "pageheap" be just an alias
// of pageheap_memory.
#define pageheap ((TCMalloc_PageHeap*) pageheap_memory)

// Thread-specific key.  Initialization here is somewhat tricky
// because some Linux startup code invokes malloc() before it
// is in a good enough state to handle pthread_keycreate().
// Therefore, we use TSD keys only after tsd_inited is set to true.
// Until then, we use a slow path to get the heap object.
static bool tsd_inited = false;
static pthread_key_t heap_key;

// Allocator for thread heaps
static PageHeapAllocator<TCMalloc_ThreadCache> threadheap_allocator;

// Linked list of heap objects.  Protected by pageheap_lock.
static TCMalloc_ThreadCache* thread_heaps = NULL;
static int thread_heap_count = 0;

// Overall thread cache size.  Protected by pageheap_lock.
static size_t overall_thread_cache_size = kDefaultOverallThreadCacheSize;

// Global per-thread cache size.  Writes are protected by
// pageheap_lock.  Reads are done without any locking, which should be
// fine as long as size_t can be written atomically and we don't place
// invariants between this variable and other pieces of state.
static volatile size_t per_thread_cache_size = kMaxThreadCacheSize;

//-------------------------------------------------------------------
// Central cache implementation
//-------------------------------------------------------------------

void TCMalloc_Central_FreeList::Init(size_t cl) {
  lock_.Init();
  size_class_ = cl;
  DLL_Init(&empty_);
  DLL_Init(&nonempty_);
  counter_ = 0;

  cache_size_ = 1;
  used_slots_ = 0;
  ASSERT(cache_size_ <= kNumTransferEntries);
}

void TCMalloc_Central_FreeList::ReleaseListToSpans(void* start) {
  while (start) {
    void *next = SLL_Next(start);
    ReleaseToSpans(start);
    start = next;
  }
}

inline void TCMalloc_Central_FreeList::ReleaseToSpans(void* object) {
  const PageID p = reinterpret_cast<uintptr_t>(object) >> kPageShift;
  Span* span = pageheap->GetDescriptor(p);
  ASSERT(span != NULL);
  ASSERT(span->refcount > 0);

  // If span is empty, move it to non-empty list
  if (span->objects == NULL) {
    DLL_Remove(span);
    DLL_Prepend(&nonempty_, span);
    Event(span, 'N', 0);
  }

  // The following check is expensive, so it is disabled by default
  if (false) {
    // Check that object does not occur in list
    int got = 0;
    for (void* p = span->objects; p != NULL; p = *((void**) p)) {
      ASSERT(p != object);
      got++;
    }
    ASSERT(got + span->refcount ==
           (span->length<<kPageShift)/ByteSizeForClass(span->sizeclass));
  }

  counter_++;
  span->refcount--;
  if (span->refcount == 0) {
    Event(span, '#', 0);
    counter_ -= (span->length<<kPageShift) / ByteSizeForClass(span->sizeclass);
    DLL_Remove(span);

    // Release central list lock while operating on pageheap
    lock_.Unlock();
    {
      SpinLockHolder h(&pageheap_lock);
      pageheap->Delete(span);
    }
    lock_.Lock();
  } else {
    *(reinterpret_cast<void**>(object)) = span->objects;
    span->objects = object;
  }
}

bool TCMalloc_Central_FreeList::EvictRandomSizeClass(
    unsigned int locked_size_class, bool force) {
  static unsigned int race_counter = 0;
  unsigned int t = race_counter++;  // Updated without a lock, but who cares.
  if (t >= kNumClasses) {
    while (t >= kNumClasses) {
      t -= kNumClasses;
    }
    race_counter = t;
  }
//  ASSERT(t >= 0);
  ASSERT(t < kNumClasses);
  if (t == locked_size_class) return false;
  return central_cache[t].ShrinkCache(locked_size_class, force);
}

bool TCMalloc_Central_FreeList::MakeCacheSpace() {
  // Is there room in the cache?
  if (used_slots_ < cache_size_) return true;
  // Check if we can expand this cache?
  if (cache_size_ == kNumTransferEntries) return false;
  // Ok, we'll try to grab an entry from some other size class.
  if (EvictRandomSizeClass(size_class_, false) ||
      EvictRandomSizeClass(size_class_, true)) {
    // Succeeded in evicting, we're going to make our cache larger.
    cache_size_++;
    return true;
  }
  return false;
}


namespace {
class LockInverter {
 private:
  TCMalloc_SpinLock *held_, *temp_;
 public:
  inline explicit LockInverter(TCMalloc_SpinLock* held, TCMalloc_SpinLock *temp)
    : held_(held), temp_(temp) { held_->Unlock(); temp_->Lock(); }
  inline ~LockInverter() { temp_->Unlock(); held_->Lock();  }
};
}

bool TCMalloc_Central_FreeList::ShrinkCache(int locked_size_class, bool force) {
  // Start with a quick check without taking a lock.
  if (cache_size_ == 0) return false;
  // We don't evict from a full cache unless we are 'forcing'.
  if (force == false && used_slots_ == cache_size_) return false;

  // Grab lock, but first release the other lock held by this thread.  We use
  // the lock inverter to ensure that we never hold two size class locks
  // concurrently.  That can create a deadlock because there is no well
  // defined nesting order.
  LockInverter li(&central_cache[locked_size_class].lock_, &lock_);
  ASSERT(used_slots_ <= cache_size_);
  ASSERT(0 <= cache_size_);
  if (cache_size_ == 0) return false;
  if (used_slots_ == cache_size_) {
    if (force == false) return false;
    // ReleaseListToSpans releases the lock, so we have to make all the
    // updates to the central list before calling it.
    cache_size_--;
    used_slots_--;
    ReleaseListToSpans(tc_slots_[used_slots_].head);
    return true;
  }
  cache_size_--;
  return true;
}

void TCMalloc_Central_FreeList::InsertRange(void *start, void *end, int N) {
  SpinLockHolder h(&lock_);
  if (N == num_objects_to_move[size_class_] &&
    MakeCacheSpace()) {
    int slot = used_slots_++;
    ASSERT(slot >=0);
    ASSERT(slot < kNumTransferEntries);
    TCEntry *entry = &tc_slots_[slot];
    entry->head = start;
    entry->tail = end;
    return;
  }
  ReleaseListToSpans(start);
}

void TCMalloc_Central_FreeList::RemoveRange(void **start, void **end, int *N) {
  int num = *N;
  ASSERT(num > 0);

  SpinLockHolder h(&lock_);
  if (num == num_objects_to_move[size_class_] && used_slots_ > 0) {
    int slot = --used_slots_;
    ASSERT(slot >= 0);
    TCEntry *entry = &tc_slots_[slot];
    *start = entry->head;
    *end = entry->tail;
    return;
  }

  // TODO: Prefetch multiple TCEntries?
  void *tail = FetchFromSpansSafe();
  if (!tail) {
    // We are completely out of memory.
    *start = *end = NULL;
    *N = 0;
    return;
  }

  SLL_SetNext(tail, NULL);
  void *head = tail;
  int count = 1;
  while (count < num) {
    void *t = FetchFromSpans();
    if (!t) break;
    SLL_Push(&head, t);
    count++;
  }
  *start = head;
  *end = tail;
  *N = count;
}


void* TCMalloc_Central_FreeList::FetchFromSpansSafe() {
  void *t = FetchFromSpans();
  if (!t) {
    Populate();
    t = FetchFromSpans();
  }
  return t;
}

inline void* TCMalloc_Central_FreeList::FetchFromSpans() {
  if (DLL_IsEmpty(&nonempty_)) return NULL;
  Span* span = nonempty_.next;

  ASSERT(span->objects != NULL);
  span->refcount++;
  void* result = span->objects;
  span->objects = *(reinterpret_cast<void**>(result));
  if (span->objects == NULL) {
    // Move to empty list
    DLL_Remove(span);
    DLL_Prepend(&empty_, span);
    Event(span, 'E', 0);
  }
  counter_--;
  return result;
}

// Fetch memory from the system and add to the central cache freelist.
inline void TCMalloc_Central_FreeList::Populate() {
  // Release central list lock while operating on pageheap
  lock_.Unlock();
  const size_t npages = class_to_pages[size_class_];

  Span* span;
  {
    SpinLockHolder h(&pageheap_lock);
    span = pageheap->New(npages);
    if (span) pageheap->RegisterSizeClass(span, size_class_);
  }
  if (span == NULL) {
    MESSAGE("allocation failed: %d\n", errno);
    lock_.Lock();
    return;
  }

  // Split the block into pieces and add to the free-list
  // TODO: coloring of objects to avoid cache conflicts?
  void** tail = &span->objects;
  char* ptr = reinterpret_cast<char*>(span->start << kPageShift);
  char* limit = ptr + (npages << kPageShift);
  const size_t size = ByteSizeForClass(size_class_);
  int num = 0;
  while (ptr + size <= limit) {
    *tail = ptr;
    tail = reinterpret_cast<void**>(ptr);
    ptr += size;
    num++;
  }
  ASSERT(ptr <= limit);
  *tail = NULL;
  span->refcount = 0; // No sub-object in use yet

  // Add span to list of non-empty spans
  lock_.Lock();
  DLL_Prepend(&nonempty_, span);
  counter_ += num;
}

//-------------------------------------------------------------------
// TCMalloc_ThreadCache implementation
//-------------------------------------------------------------------

inline bool TCMalloc_ThreadCache::SampleAllocation(size_t k) {
  if (bytes_until_sample_ < k) {
    PickNextSample();
    return true;
  } else {
    bytes_until_sample_ -= k;
    return false;
  }
}

void TCMalloc_ThreadCache::Init(pthread_t tid) {
  size_ = 0;
  next_ = NULL;
  prev_ = NULL;
  tid_  = tid;
  in_setspecific_ = false;
  for (size_t cl = 0; cl < kNumClasses; ++cl) {
    list_[cl].Init();
  }

  // Initialize RNG -- run it for a bit to get to good values
  rnd_ = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
  for (int i = 0; i < 100; i++) {
    PickNextSample();
  }
}

void TCMalloc_ThreadCache::Cleanup() {
  // Put unused memory back into central cache
  for (unsigned int cl = 0; cl < kNumClasses; ++cl) {
    if (list_[cl].length() > 0) {
      ReleaseToCentralCache(cl, list_[cl].length());
    }
  }
}

inline void* TCMalloc_ThreadCache::Allocate(size_t size) {
  ASSERT(size <= kMaxSize);
  const size_t cl = SizeClass(size);
  FreeList* list = &list_[cl];
  if (list->empty()) {
    FetchFromCentralCache(cl);
    if (list->empty()) return NULL;
  }
  size_ -= ByteSizeForClass(cl);
  return list->Pop();
}

inline void TCMalloc_ThreadCache::Deallocate(void* ptr, size_t cl) {
  size_ += ByteSizeForClass(cl);
  FreeList* list = &list_[cl];
  list->Push(ptr);
  // If enough data is free, put back into central cache
  if (list->length() > kMaxFreeListLength) {
    ReleaseToCentralCache(cl, num_objects_to_move[cl]);
  }
  if (size_ >= per_thread_cache_size) Scavenge();
}

// Remove some objects of class "cl" from central cache and add to thread heap
inline void TCMalloc_ThreadCache::FetchFromCentralCache(size_t cl) {
  int fetch_count = num_objects_to_move[cl];
  void *start, *end;
  central_cache[cl].RemoveRange(&start, &end, &fetch_count);
  list_[cl].PushRange(fetch_count, start, end);
  size_ += ByteSizeForClass(cl) * fetch_count;
}

// Remove some objects of class "cl" from thread heap and add to central cache
void TCMalloc_ThreadCache::ReleaseToCentralCache(size_t cl, int N) {
  ASSERT(N > 0);
  FreeList* src = &list_[cl];
  if (N > src->length()) N = src->length();
  size_ -= N*ByteSizeForClass(cl);

  // We return prepackaged chains of the correct size to the central cache.
  // TODO: Use the same format internally in the thread caches?
  int batch_size = num_objects_to_move[cl];
  while (N > batch_size) {
    void *tail, *head;
    src->PopRange(batch_size, &head, &tail);
    central_cache[cl].InsertRange(head, tail, batch_size);
    N -= batch_size;
  }
  void *tail, *head;
  src->PopRange(N, &head, &tail);
  central_cache[cl].InsertRange(head, tail, N);
}

// Release idle memory to the central cache
inline void TCMalloc_ThreadCache::Scavenge() {
  // If the low-water mark for the free list is L, it means we would
  // not have had to allocate anything from the central cache even if
  // we had reduced the free list size by L.  We aim to get closer to
  // that situation by dropping L/2 nodes from the free list.  This
  // may not release much memory, but if so we will call scavenge again
  // pretty soon and the low-water marks will be high on that call.
  //int64 start = CycleClock::Now();

  for (unsigned int cl = 0; cl < kNumClasses; cl++) {
    FreeList* list = &list_[cl];
    const int lowmark = list->lowwatermark();
    if (lowmark > 0) {
      const int drop = (lowmark > 1) ? lowmark/2 : 1;
      ReleaseToCentralCache(cl, drop);
    }
    list->clear_lowwatermark();
  }

  //int64 finish = CycleClock::Now();
  //CycleTimer ct;
  //MESSAGE("GC: %.0f ns\n", ct.CyclesToUsec(finish-start)*1000.0);
}

inline TCMalloc_ThreadCache* TCMalloc_ThreadCache::GetCache() {
  void* ptr = NULL;
  if (!tsd_inited) {
    InitModule();
  } else {
    ptr = pthread_getspecific(heap_key);
  }
  if (ptr == NULL) ptr = CreateCacheIfNecessary();
  return reinterpret_cast<TCMalloc_ThreadCache*>(ptr);
}

// In deletion paths, we do not try to create a thread-cache.  This is
// because we may be in the thread destruction code and may have
// already cleaned up the cache for this thread.
inline TCMalloc_ThreadCache* TCMalloc_ThreadCache::GetCacheIfPresent() {
  if (!tsd_inited) return NULL;
  return reinterpret_cast<TCMalloc_ThreadCache*>
    (pthread_getspecific(heap_key));
}

void TCMalloc_ThreadCache::PickNextSample() {
  // Make next "random" number
  // x^32+x^22+x^2+x^1+1 is a primitive polynomial for random numbers
  static const uint32_t kPoly = (1 << 22) | (1 << 2) | (1 << 1) | (1 << 0);
  uint32_t r = rnd_;
  rnd_ = (r << 1) ^ ((static_cast<int32_t>(r) >> 31) & kPoly);

  // Next point is "rnd_ % (2*sample_period)".  I.e., average
  // increment is "sample_period".
  bytes_until_sample_ = rnd_ % kSampleParameter;
}

void TCMalloc_ThreadCache::InitModule() {
  // There is a slight potential race here because of double-checked
  // locking idiom.  However, as long as the program does a small
  // allocation before switching to multi-threaded mode, we will be
  // fine.  We increase the chances of doing such a small allocation
  // by doing one in the constructor of the module_enter_exit_hook
  // object declared below.
  SpinLockHolder h(&pageheap_lock);
  if (!phinited) {
    if (TCMallocDebug::level >= TCMallocDebug::kInfo) {
      MESSAGE("TCMalloc_ThreadCache::InitModule()\n");
    }
    InitSizeClasses();
    threadheap_allocator.Init();
    span_allocator.Init();
    span_allocator.New(); // Reduce cache conflicts
    span_allocator.New(); // Reduce cache conflicts
    stacktrace_allocator.Init();
    DLL_Init(&sampled_objects);
    for (unsigned int i = 0; i < kNumClasses; ++i) {
      central_cache[i].Init(i);
    }
    new ((void*)pageheap_memory) TCMalloc_PageHeap;
    phinited = 1;
  }
}

void TCMalloc_ThreadCache::InitTSD() {
  ASSERT(!tsd_inited);
  pthread_key_create(&heap_key, DeleteCache);
  tsd_inited = true;

  // We may have used a fake pthread_t for the main thread.  Fix it.
  pthread_t zero;
  memset(&zero, 0, sizeof(zero));
  SpinLockHolder h(&pageheap_lock);
  for (TCMalloc_ThreadCache* h = thread_heaps; h != NULL; h = h->next_) {
    if (h->tid_ == zero) {
      h->tid_ = pthread_self();
    }
  }
}

void* TCMalloc_ThreadCache::CreateCacheIfNecessary() {
  // Initialize per-thread data if necessary
  TCMalloc_ThreadCache* heap = NULL;
  {
    SpinLockHolder h(&pageheap_lock);

    // Early on in glibc's life, we cannot even call pthread_self()
    pthread_t me;
    if (!tsd_inited) {
      memset(&me, 0, sizeof(me));
    } else {
      me = pthread_self();
    }

    // This may be a recursive malloc call from pthread_setspecific()
    // In that case, the heap for this thread has already been created
    // and added to the linked list.  So we search for that first.
    for (TCMalloc_ThreadCache* h = thread_heaps; h != NULL; h = h->next_) {
      if (h->tid_ == me) {
        heap = h;
        break;
      }
    }

    if (heap == NULL) {
      // Create the heap and add it to the linked list
      heap = threadheap_allocator.New();
      heap->Init(me);
      heap->next_ = thread_heaps;
      heap->prev_ = NULL;
      if (thread_heaps != NULL) thread_heaps->prev_ = heap;
      thread_heaps = heap;
      thread_heap_count++;
      RecomputeThreadCacheSize();
    }
  }

  // We call pthread_setspecific() outside the lock because it may
  // call malloc() recursively.  We check for the recursive call using
  // the "in_setspecific_" flag so that we can avoid calling
  // pthread_setspecific() if we are already inside pthread_setspecific().
  if (!heap->in_setspecific_ && tsd_inited) {
    heap->in_setspecific_ = true;
    pthread_setspecific(heap_key, heap);
    heap->in_setspecific_ = false;
  }
  return heap;
}

void TCMalloc_ThreadCache::DeleteCache(void* ptr) {
  // Remove all memory from heap
  TCMalloc_ThreadCache* heap;
  heap = reinterpret_cast<TCMalloc_ThreadCache*>(ptr);
  heap->Cleanup();

  // Remove from linked list
  SpinLockHolder h(&pageheap_lock);
  if (heap->next_ != NULL) heap->next_->prev_ = heap->prev_;
  if (heap->prev_ != NULL) heap->prev_->next_ = heap->next_;
  if (thread_heaps == heap) thread_heaps = heap->next_;
  thread_heap_count--;
  RecomputeThreadCacheSize();

  threadheap_allocator.Delete(heap);
}

void TCMalloc_ThreadCache::RecomputeThreadCacheSize() {
  // Divide available space across threads
  int n = thread_heap_count > 0 ? thread_heap_count : 1;
  size_t space = overall_thread_cache_size / n;

  // Limit to allowed range
  if (space < kMinThreadCacheSize) space = kMinThreadCacheSize;
  if (space > kMaxThreadCacheSize) space = kMaxThreadCacheSize;

  per_thread_cache_size = space;
}

void TCMalloc_ThreadCache::Print() const {
  for (unsigned int cl = 0; cl < kNumClasses; ++cl) {
    MESSAGE("      %5" PRIuS " : %4d len; %4d lo\n",
            ByteSizeForClass(cl),
            list_[cl].length(),
            list_[cl].lowwatermark());
  }
}

// Extract interesting stats
struct TCMallocStats {
  uint64_t system_bytes;        // Bytes alloced from system
  uint64_t thread_bytes;        // Bytes in thread caches
  uint64_t central_bytes;       // Bytes in central cache
  uint64_t transfer_bytes;      // Bytes in central transfer cache
  uint64_t pageheap_bytes;      // Bytes in page heap
  uint64_t metadata_bytes;      // Bytes alloced for metadata
};

//// Get stats into "r".  Also get per-size-class counts if class_count != NULL
//static void ExtractStats(TCMallocStats* r, uint64_t* class_count) {
//  r->central_bytes = 0;
//  r->transfer_bytes = 0;
//  for (int cl = 0; cl < kNumClasses; ++cl) {
//    const int length = central_cache[cl].length();
//    const int tc_length = central_cache[cl].tc_length();
//    r->central_bytes += static_cast<uint64_t>(ByteSizeForClass(cl)) * length;
//    r->transfer_bytes +=
//      static_cast<uint64_t>(ByteSizeForClass(cl)) * tc_length;
//    if (class_count) class_count[cl] = length + tc_length;
//  }
//
//  // Add stats from per-thread heaps
//  r->thread_bytes = 0;
//  { // scope
//    SpinLockHolder h(&pageheap_lock);
//    for (TCMalloc_ThreadCache* h = thread_heaps; h != NULL; h = h->next_) {
//      r->thread_bytes += h->Size();
//      if (class_count) {
//        for (int cl = 0; cl < kNumClasses; ++cl) {
//          class_count[cl] += h->freelist_length(cl);
//        }
//      }
//    }
//  }
//
//  { //scope
//    SpinLockHolder h(&pageheap_lock);
//    r->system_bytes = pageheap->SystemBytes();
//    r->metadata_bytes = metadata_system_bytes;
//    r->pageheap_bytes = pageheap->FreeBytes();
//  }
//}

//// WRITE stats to "out"
//static void DumpStats(TCMalloc_Printer* out, int level) {
//  TCMallocStats stats;
//  uint64_t class_count[kNumClasses];
//  ExtractStats(&stats, (level >= 2 ? class_count : NULL));
//
//  if (level >= 2) {
//    out->printf("------------------------------------------------\n");
//    uint64_t cumulative = 0;
//    for (int cl = 0; cl < kNumClasses; ++cl) {
//      if (class_count[cl] > 0) {
//        uint64_t class_bytes = class_count[cl] * ByteSizeForClass(cl);
//        cumulative += class_bytes;
//        out->printf("class %3d [ %8" PRIuS " bytes ] : "
//                "%8" LLU " objs; %5.1f MB; %5.1f cum MB\n",
//                cl, ByteSizeForClass(cl),
//                class_count[cl],
//                class_bytes / 1048576.0,
//                cumulative / 1048576.0);
//      }
//    }
//
//    SpinLockHolder h(&pageheap_lock);
//    pageheap->Dump(out);
//  }
//
//  const uint64_t bytes_in_use = stats.system_bytes
//                                - stats.pageheap_bytes
//                                - stats.central_bytes
//                                - stats.transfer_bytes
//                                - stats.thread_bytes;
//
//  out->printf("------------------------------------------------\n"
//              "MALLOC: %12" LLU " Heap size\n"
//              "MALLOC: %12" LLU " Bytes in use by application\n"
//              "MALLOC: %12" LLU " Bytes free in page heap\n"
//              "MALLOC: %12" LLU " Bytes free in central cache\n"
//              "MALLOC: %12" LLU " Bytes free in transfer cache\n"
//              "MALLOC: %12" LLU " Bytes free in thread caches\n"
//              "MALLOC: %12" LLU " Spans in use\n"
//              "MALLOC: %12" LLU " Thread heaps in use\n"
//              "MALLOC: %12" LLU " Metadata allocated\n"
//              "------------------------------------------------\n",
//              stats.system_bytes,
//              bytes_in_use,
//              stats.pageheap_bytes,
//              stats.central_bytes,
//              stats.transfer_bytes,
//              stats.thread_bytes,
//              uint64_t(span_allocator.inuse()),
//              uint64_t(threadheap_allocator.inuse()),
//              stats.metadata_bytes);
//}
//
//static void PrintStats(int level) {
//  const int kBufferSize = 16 << 10;
//  char* buffer = new char[kBufferSize];
//  TCMalloc_Printer printer(buffer, kBufferSize);
//  DumpStats(&printer, level);
//  write(STDERR_FILENO, buffer, strlen(buffer));
//  delete[] buffer;
//}
//
//static void** DumpStackTraces() {
//  // Count how much space we need
//  int needed_slots = 0;
//  {
//    SpinLockHolder h(&pageheap_lock);
//    for (Span* s = sampled_objects.next; s != &sampled_objects; s = s->next) {
//      StackTrace* stack = reinterpret_cast<StackTrace*>(s->objects);
//      needed_slots += 3 + stack->depth;
//    }
//    needed_slots += 100;            // Slop in case sample grows
//    needed_slots += needed_slots/8; // An extra 12.5% slop
//  }
//
//  void** result = new void*[needed_slots];
//  if (result == NULL) {
//    MESSAGE("tcmalloc: could not allocate %d slots for stack traces\n",
//            needed_slots);
//    return NULL;
//  }
//
//  SpinLockHolder h(&pageheap_lock);
//  int used_slots = 0;
//  for (Span* s = sampled_objects.next; s != &sampled_objects; s = s->next) {
//    ASSERT(used_slots < needed_slots);  // Need to leave room for terminator
//    StackTrace* stack = reinterpret_cast<StackTrace*>(s->objects);
//    if (used_slots + 3 + stack->depth >= needed_slots) {
//      // No more room
//      break;
//    }
//
//    result[used_slots+0] = reinterpret_cast<void*>(1);
//    result[used_slots+1] = reinterpret_cast<void*>(stack->size);
//    result[used_slots+2] = reinterpret_cast<void*>(stack->depth);
//    for (int d = 0; d < stack->depth; d++) {
//      result[used_slots+3+d] = stack->stack[d];
//    }
//    used_slots += 3 + stack->depth;
//  }
//  result[used_slots] = reinterpret_cast<void*>(0);
//  return result;
//}
//
//static void** DumpHeapGrowthStackTraces() {
//  // Count how much space we need
//  int needed_slots = 0;
//  {
//    SpinLockHolder h(&pageheap_lock);
//    for (StackTrace* t = growth_stacks;
//         t != NULL;
//         t = reinterpret_cast<StackTrace*>(t->stack[kMaxStackDepth-1])) {
//      needed_slots += 3 + t->depth;
//    }
//    needed_slots += 100;            // Slop in case list grows
//    needed_slots += needed_slots/8; // An extra 12.5% slop
//  }
//
//  void** result = new void*[needed_slots];
//  if (result == NULL) {
//    MESSAGE("tcmalloc: could not allocate %d slots for stack traces\n",
//            needed_slots);
//    return NULL;
//  }
//
//  SpinLockHolder h(&pageheap_lock);
//  int used_slots = 0;
//  for (StackTrace* t = growth_stacks;
//       t != NULL;
//       t = reinterpret_cast<StackTrace*>(t->stack[kMaxStackDepth-1])) {
//    ASSERT(used_slots < needed_slots);  // Need to leave room for terminator
//    if (used_slots + 3 + t->depth >= needed_slots) {
//      // No more room
//      break;
//    }
//
//    result[used_slots+0] = reinterpret_cast<void*>(1);
//    result[used_slots+1] = reinterpret_cast<void*>(t->size);
//    result[used_slots+2] = reinterpret_cast<void*>(t->depth);
//    for (int d = 0; d < t->depth; d++) {
//      result[used_slots+3+d] = t->stack[d];
//    }
//    used_slots += 3 + t->depth;
//  }
//  result[used_slots] = reinterpret_cast<void*>(0);
//  return result;
//}

//-------------------------------------------------------------------
// Helpers for the exported routines below
//-------------------------------------------------------------------

// @not_used -static Span* DoSampledAllocation(size_t size) {
// @not_used -  SpinLockHolder h(&pageheap_lock);
// @not_used -
// @not_used -  // Allocate span
// @not_used -  Span* span = pageheap->New(pages(size == 0 ? 1 : size));
// @not_used -  if (span == NULL) {
// @not_used -    return NULL;
// @not_used -  }
// @not_used -
// @not_used -  // Allocate stack trace
// @not_used -  StackTrace* stack = stacktrace_allocator.New();
// @not_used -  if (stack == NULL) {
// @not_used -    // Sampling failed because of lack of memory
// @not_used -    return span;
// @not_used -  }
// @not_used -
// @not_used -  // Fill stack trace and record properly
// @not_used -  stack->depth = GetStackTrace(stack->stack, kMaxStackDepth, 1);
// @not_used -  stack->size = size;
// @not_used -  span->sample = 1;
// @not_used -  span->objects = stack;
// @not_used -  DLL_Prepend(&sampled_objects, span);
// @not_used -
// @not_used -  return span;
// @not_used -}

static inline void* do_malloc(size_t size) {
  void* ret = NULL;

  if (TCMallocDebug::level >= TCMallocDebug::kVerbose) {
    MESSAGE("In tcmalloc do_malloc(%" PRIuS")\n", size);
  }
// @not_used -  // The following call forces module initialization
// @not_used -  TCMalloc_ThreadCache* heap = TCMalloc_ThreadCache::GetCache();
// @not_used -  if (heap->SampleAllocation(size)) {
// @not_used -    Span* span = DoSampledAllocation(size);
// @not_used -    if (span != NULL) {
// @not_used -      ret = reinterpret_cast<void*>(span->start << kPageShift);
// @not_used -    }
// @not_used -  } else 
  if (size > kMaxSize) {
    // Use page-level allocator
    SpinLockHolder h(&pageheap_lock);
    Span* span = pageheap->New(pages(size));
    if (span != NULL) {
      ret = reinterpret_cast<void*>(span->start << kPageShift);
    }
  } else {
    TCMalloc_ThreadCache* heap = TCMalloc_ThreadCache::GetCache();
    ret = heap->Allocate(size);
  }
  if (ret == NULL) errno = ENOMEM;
  return ret;
}

static inline void do_free(void* ptr) {
  if (TCMallocDebug::level >= TCMallocDebug::kVerbose)
    MESSAGE("In tcmalloc do_free(%p)\n", ptr);
  if (ptr == NULL) return;
  ASSERT(pageheap != NULL);  // Should not call free() before malloc()
  const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
  Span* span = pageheap->GetDescriptor(p);

  ASSERT(span != NULL);
  ASSERT(!span->free);
  const size_t cl = span->sizeclass;
  if (cl != 0) {
    ASSERT(!span->sample);
    TCMalloc_ThreadCache* heap = TCMalloc_ThreadCache::GetCacheIfPresent();
    if (heap != NULL) {
      heap->Deallocate(ptr, cl);
    } else {
      // Delete directly into central cache
      SLL_SetNext(ptr, NULL);
      central_cache[cl].InsertRange(ptr, ptr, 1);
    }
  } else {
    SpinLockHolder h(&pageheap_lock);
    ASSERT(reinterpret_cast<uintptr_t>(ptr) % kPageSize == 0);
    ASSERT(span->start == p);
    if (span->sample) {
      DLL_Remove(span);
      stacktrace_allocator.Delete(reinterpret_cast<StackTrace*>(span->objects));
      span->objects = NULL;
    }
    pageheap->Delete(span);
  }
}

// For use by exported routines below that want specific alignments
//
// Note: this code can be slow, and can significantly fragment memory.
// The expectation is that memalign/posix_memalign/valloc/pvalloc will
// not be invoked very often.  This requirement simplifies our
// implementation and allows us to tune for expected allocation
// patterns.
static void* do_memalign(size_t align, size_t size) {
  ASSERT((align & (align - 1)) == 0);
  ASSERT(align > 0);
  if (size + align < size) return NULL;         // Overflow

  if (pageheap == NULL) TCMalloc_ThreadCache::InitModule();

  // Allocate at least one byte to avoid boundary conditions below
  if (size == 0) size = 1;

  if (size <= kMaxSize && align < kPageSize) {
    // Search through acceptable size classes looking for one with
    // enough alignment.  This depends on the fact that
    // InitSizeClasses() currently produces several size classes that
    // are aligned at powers of two.  We will waste time and space if
    // we miss in the size class array, but that is deemed acceptable
    // since memalign() should be used rarely.
    unsigned int cl = SizeClass(size);
    while (cl < kNumClasses && ((class_to_size[cl] & (align - 1)) != 0)) {
      cl++;
    }
    if (cl < kNumClasses) {
      TCMalloc_ThreadCache* heap = TCMalloc_ThreadCache::GetCache();
      return heap->Allocate(class_to_size[cl]);
    }
  }

  // We will allocate directly from the page heap
  SpinLockHolder h(&pageheap_lock);

  if (align <= kPageSize) {
    // Any page-level allocation will be fine
    // TODO: We could put the rest of this page in the appropriate
    // TODO: cache but it does not seem worth it.
    Span* span = pageheap->New(pages(size));
    if (span == NULL) return NULL;
    return reinterpret_cast<void*>(span->start << kPageShift);
  }

  // Allocate extra pages and carve off an aligned portion
  const int alloc = pages(size + align);
  Span* span = pageheap->New(alloc);
  if (span == NULL) return NULL;

  // Skip starting portion so that we end up aligned
  int skip = 0;
  while ((((span->start+skip) << kPageShift) & (align - 1)) != 0) {
    skip++;
  }
  ASSERT(skip < alloc);
  if (skip > 0) {
    Span* rest = pageheap->Split(span, skip);
    pageheap->Delete(span);
    span = rest;
  }

  // Skip trailing portion that we do not need to return
  const unsigned int needed = pages(size);
  ASSERT(span->length >= needed);
  if (span->length > needed) {
    Span* trailer = pageheap->Split(span, needed);
    pageheap->Delete(trailer);
  }
  return reinterpret_cast<void*>(span->start << kPageShift);
}



// The constructor allocates an object to ensure that initialization
// runs before main(), and therefore we do not have a chance to become
// multi-threaded before initialization.  We also create the TSD key
// here.  Presumably by the time this constructor runs, glibc is in
// good enough shape to handle pthread_key_create().
//
// The constructor also takes the opportunity to tell STL to use
// tcmalloc.  We want to do this early, before construct time, so
// all user STL allocations go through tcmalloc (which works really
// well for STL).
//
// The destructor prints stats when the program exits.

class TCMallocGuard {
 public:
  TCMallocGuard() {
    char *envval;
    if ((envval = getenv("TCMALLOC_DEBUG"))) {
      TCMallocDebug::level = atoi(envval);
      MESSAGE("Set tcmalloc debugging level to %d\n", TCMallocDebug::level);
    }
    do_free(do_malloc(1));
    TCMalloc_ThreadCache::InitTSD();
    do_free(do_malloc(1));
// @not_used -    MallocExtension::Register(new TCMallocImplementation);
  }

// @not_used -  ~TCMallocGuard() {
// @not_used -    const char* env = getenv("MALLOCSTATS");
// @not_used -    if (env != NULL) {
// @not_used -      int level = atoi(env);
// @not_used -      if (level < 1) level = 1;
// @not_used -      PrintStats(level);
// @not_used -    }
// @not_used -  }
};

static TCMallocGuard module_enter_exit_hook;


//-------------------------------------------------------------------
// Exported routines
//-------------------------------------------------------------------

// CAVEAT: The code structure below ensures that MallocHook methods are always
//         called from the stack frame of the invoked allocation function.
//         heap-checker.cc depends on this to start a stack trace from
//         the call to the (de)allocation function.

extern "C" void* malloc(size_t size) {
  void* result = do_malloc(size);
// @not_used -  MallocHook::InvokeNewHook(result, size);
  return result;
}

extern "C" void free(void* ptr) {
// @not_used -  MallocHook::InvokeDeleteHook(ptr);
  do_free(ptr);
}

extern "C" void* calloc(size_t n, size_t elem_size) {
  // Overflow check
  const size_t size = n * elem_size;
  if (elem_size != 0 && size / elem_size != n) return NULL;

  void* result = do_malloc(size);
  if (result != NULL) {
    memset(result, 0, size);
  }
// @not_used -  MallocHook::InvokeNewHook(result, size);
  return result;
}

extern "C" void cfree(void* ptr) {
// @not_used -  MallocHook::InvokeDeleteHook(ptr);
  do_free(ptr);
}

extern "C" void* realloc(void* old_ptr, size_t new_size) {
  if (old_ptr == NULL) {
    void* result = do_malloc(new_size);
// @not_used -    MallocHook::InvokeNewHook(result, new_size);
    return result;
  }
  if (new_size == 0) {
// @not_used -    MallocHook::InvokeDeleteHook(old_ptr);
    do_free(old_ptr);
    return NULL;
  }

  // Get the size of the old entry
  const PageID p = reinterpret_cast<uintptr_t>(old_ptr) >> kPageShift;
  Span* span = pageheap->GetDescriptor(p);
  size_t old_size;
  if (span->sizeclass != 0) {
    old_size = ByteSizeForClass(span->sizeclass);
  } else {
    old_size = span->length << kPageShift;
  }

  // Reallocate if the new size is larger than the old size,
  // or if the new size is significantly smaller than the old size.
  if ((new_size > old_size) || (AllocationSize(new_size) < old_size)) {
    // Need to reallocate
    void* new_ptr = do_malloc(new_size);
    if (new_ptr == NULL) {
      return NULL;
    }
// @not_used -    MallocHook::InvokeNewHook(new_ptr, new_size);
    memcpy(new_ptr, old_ptr, ((old_size < new_size) ? old_size : new_size));
// @not_used -    MallocHook::InvokeDeleteHook(old_ptr);
    do_free(old_ptr);
    return new_ptr;
  } else {
    return old_ptr;
  }
}

#ifndef COMPILER_INTEL
#define OP_THROWNOTHING
#define OP_THROWBADALLOC
#else
#define OP_THROWNOTHING throw()
#define OP_THROWBADALLOC throw(std::bad_alloc)
#endif

static SpinLock set_new_handler_lock = SPINLOCK_INITIALIZER;

static inline void* cpp_alloc(size_t size, bool nothrow) {
  for (;;) {
    void* p = do_malloc(size);
#ifdef PREANSINEW
// @not_used -    MallocHook::InvokeNewHook(p, size);
    return p;
#else
    if (p == NULL) {  // allocation failed
      // Get the current new handler.  NB: this function is not
      // thread-safe.  We make a feeble stab at making it so here, but
      // this lock only protects against tcmalloc interfering with
      // itself, not with other libraries calling set_new_handler.
      std::new_handler nh;
      {
        SpinLockHolder h(&set_new_handler_lock);
        nh = std::set_new_handler(0);
        (void) std::set_new_handler(nh);
      }
      // If no new_handler is established, the allocation failed.
      if (!nh) {
        if (nothrow) return 0;
        throw std::bad_alloc();
      }
      // Otherwise, try the new_handler.  If it returns, retry the
      // allocation.  If it throws std::bad_alloc, fail the allocation.
      // if it throws something else, don't interfere.
      try {
        (*nh)();
      } catch (const std::bad_alloc&) {
        if (!nothrow) throw;
// @not_used -        MallocHook::InvokeNewHook(p, size);
        return p;
      }
    } else {  // allocation success
// @not_used -      MallocHook::InvokeNewHook(p, size);
      return p;
    }
#endif
  }
}

void* operator new(size_t size) OP_THROWBADALLOC {
  return cpp_alloc(size, false);
}

void* operator new(size_t size, const std::nothrow_t&) OP_THROWNOTHING {
  return cpp_alloc(size, true);
}

void operator delete(void* p) OP_THROWNOTHING {
// @not_used -  MallocHook::InvokeDeleteHook(p);
  do_free(p);
}

void operator delete(void* p, const std::nothrow_t&) OP_THROWNOTHING {
// @not_used -  MallocHook::InvokeDeleteHook(p);
  do_free(p);
}

void* operator new[](size_t size) OP_THROWBADALLOC {
  return cpp_alloc(size, false);
}

void* operator new[](size_t size, const std::nothrow_t&) OP_THROWNOTHING {
  return cpp_alloc(size, true);
}

void operator delete[](void* p) OP_THROWNOTHING {
// @not_used -  MallocHook::InvokeDeleteHook(p);
  do_free(p);
}

void operator delete[](void* p, const std::nothrow_t&) OP_THROWNOTHING {
// @not_used -  MallocHook::InvokeDeleteHook(p);
  do_free(p);
}

extern "C" void* memalign(size_t align, size_t size) {
  void* result = do_memalign(align, size);
// @not_used -  MallocHook::InvokeNewHook(result, size);
  return result;
}

extern "C" int posix_memalign(void** result_ptr, size_t align, size_t size) {
  if (((align % sizeof(void*)) != 0) ||
      ((align & (align - 1)) != 0) ||
      (align == 0)) {
    return EINVAL;
  }

  void* result = do_memalign(align, size);
// @not_used -  MallocHook::InvokeNewHook(result, size);
  if (result == NULL) {
    return ENOMEM;
  } else {
    *result_ptr = result;
    return 0;
  }
}

static size_t pagesize = 0;

extern "C" void* valloc(size_t size) {
  // Allocate page-aligned object of length >= size bytes
  if (pagesize == 0) pagesize = getpagesize();
  void* result = do_memalign(pagesize, size);
// @not_used -  MallocHook::InvokeNewHook(result, size);
  return result;
}

extern "C" void* pvalloc(size_t size) {
  // Round up size to a multiple of pagesize
  if (pagesize == 0) pagesize = getpagesize();
  size = (size + pagesize - 1) & ~(pagesize - 1);
  void* result = do_memalign(pagesize, size);
// @not_used -  MallocHook::InvokeNewHook(result, size);
  return result;
}

extern "C" void malloc_stats(void) {
// @not_used -  PrintStats(1);
}

extern "C" int mallopt(int cmd, int value) {
  return 1;     // Indicates error
}

// @not_used -extern "C" struct mallinfo mallinfo(void) {
// @not_used -  TCMallocStats stats;
// @not_used -  ExtractStats(&stats, NULL);
// @not_used -
// @not_used -  // Just some of the fields are filled in.
// @not_used -  struct mallinfo info;
// @not_used -  memset(&info, 0, sizeof(info));
// @not_used -
// @not_used -  // Unfortunately, the struct contains "int" field, so some of the
// @not_used -  // size values will be truncated.
// @not_used -  info.arena     = static_cast<int>(stats.system_bytes);
// @not_used -  info.fsmblks   = static_cast<int>(stats.thread_bytes
// @not_used -                                    + stats.central_bytes
// @not_used -                                    + stats.transfer_bytes);
// @not_used -  info.fordblks  = static_cast<int>(stats.pageheap_bytes);
// @not_used -  info.uordblks  = static_cast<int>(stats.system_bytes
// @not_used -                                    - stats.thread_bytes
// @not_used -                                    - stats.central_bytes
// @not_used -                                    - stats.transfer_bytes
// @not_used -                                    - stats.pageheap_bytes);
// @not_used -
// @not_used -  return info;
// @not_used -}

//-------------------------------------------------------------------
// Some library routines on RedHat 9 allocate memory using malloc()
// and free it using __libc_free() (or vice-versa).  Since we provide
// our own implementations of malloc/free, we need to make sure that
// the __libc_XXX variants also point to the same implementations.
//-------------------------------------------------------------------

extern "C" {
#if defined(__GNUC__) && defined(HAVE___ATTRIBUTE__)
  // Potentially faster variants that use the gcc alias extension
#define ALIAS(x) __attribute__ ((weak, alias (x)))
  void* __libc_malloc(size_t size)              ALIAS("malloc");
  void  __libc_free(void* ptr)                  ALIAS("free");
  void* __libc_realloc(void* ptr, size_t size)  ALIAS("realloc");
  void* __libc_calloc(size_t n, size_t size)    ALIAS("calloc");
  void  __libc_cfree(void* ptr)                 ALIAS("cfree");
  void* __libc_memalign(size_t align, size_t s) ALIAS("memalign");
  void* __libc_valloc(size_t size)              ALIAS("valloc");
  void* __libc_pvalloc(size_t size)             ALIAS("pvalloc");
  int __posix_memalign(void** r, size_t a, size_t s) ALIAS("posix_memalign");
#undef ALIAS
#else
  // Portable wrappers
  void* __libc_malloc(size_t size)              { return malloc(size);       }
  void  __libc_free(void* ptr)                  { free(ptr);                 }
  void* __libc_realloc(void* ptr, size_t size)  { return realloc(ptr, size); }
  void* __libc_calloc(size_t n, size_t size)    { return calloc(n, size);    }
  void  __libc_cfree(void* ptr)                 { cfree(ptr);                }
  void* __libc_memalign(size_t align, size_t s) { return memalign(align, s); }
  void* __libc_valloc(size_t size)              { return valloc(size);       }
  void* __libc_pvalloc(size_t size)             { return pvalloc(size);      }
  int __posix_memalign(void** r, size_t a, size_t s) {
    return posix_memalign(r, a, s);
  }
#endif
}

// Override __libc_memalign in libc on linux boxes specially.
// They have a bug in libc that causes them to (very rarely) allocate
// with __libc_memalign() yet deallocate with free() and the
// definitions above don't catch it.
// This function is an exception to the rule of calling MallocHook method
// from the stack frame of the allocation function;
// heap-checker handles this special case explicitly.

// @not_used -static void *MemalignOverride(size_t align, size_t size, const void *caller) {
// @not_used -  void* result = do_memalign(align, size);
// @not_used -  MallocHook::InvokeNewHook(result, size);
// @not_used -  return result;
// @not_used -}
// @not_used -void *(*__memalign_hook)(size_t, size_t, const void *) = MemalignOverride;
