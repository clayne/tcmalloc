// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/system-alloc.h"

#include <asm/unistd.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "absl/base/attributes.h"
#include "absl/base/call_once.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"
#include "absl/types/span.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/parameters.h"

// On systems (like freebsd) that don't define MAP_ANONYMOUS, use the old
// form of the name instead.
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef MADV_FREE
#define MADV_FREE 8
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// Solaris has a bug where it doesn't declare madvise() for C++.
//    http://www.opensolaris.org/jive/thread.jspa?threadID=21035&tstart=0
#if defined(__sun) && defined(__SVR4)
#include <sys/types.h>
extern "C" int madvise(caddr_t, size_t, int);
#endif

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc::tcmalloc_internal::system_allocator_internal {

// Structure for discovering alignment
union MemoryAligner {
  void* p;
  double d;
  size_t s;
} ABSL_CACHELINE_ALIGNED;

static_assert(sizeof(MemoryAligner) < kHugePageSize,
              "hugepage alignment too small");

int MapFixedNoReplaceFlagAvailable() {
  ABSL_CONST_INIT static int noreplace_flag;
  ABSL_CONST_INIT static absl::once_flag flag;

  absl::base_internal::LowLevelCallOnce(&flag, [&]() {
    void* ptr =
        mmap(nullptr, kPageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    TC_CHECK_NE(ptr, MAP_FAILED);

    // Try to map over ptr.  If we get a different address, we don't have
    // MAP_FIXED_NOREPLACE.
    //
    // We try to specify a region that overlaps with ptr, but adjust the start
    // downward so it doesn't.  This allows us to detect if the pre-4.19 bug
    // (https://github.com/torvalds/linux/commit/7aa867dd89526e9cfd9714d8b9b587c016eaea34)
    // is present.
    uintptr_t uptr = reinterpret_cast<uintptr_t>(ptr);
    TC_CHECK_GT(uptr, kPageSize);
    void* target = reinterpret_cast<void*>(uptr - kPageSize);

    void* ptr2 = mmap(target, 2 * kPageSize, PROT_NONE,
                      MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    const bool rejected = ptr2 == MAP_FAILED;
    if (!rejected) {
      if (ptr2 == target) {
        // [ptr2, 2 * kPageSize] overlaps with [ptr, kPageSize], so we only need
        // to unmap [ptr2, kPageSize].  The second call to munmap below will
        // unmap the rest.
        munmap(ptr2, kPageSize);
      } else {
        munmap(ptr2, 2 * kPageSize);
      }
    }
    munmap(ptr, kPageSize);

    noreplace_flag = rejected ? MAP_FIXED_NOREPLACE : 0;
  });

  return noreplace_flag;
}

bool ReleasePages(void* start, size_t length) {
  ErrnoRestorer errno_restorer;

  int ret;
  // Note -- ignoring most return codes, because if this fails it
  // doesn't matter...
  // Moreover, MADV_REMOVE *will* fail (with EINVAL) on private memory,
  // but that's harmless.
#ifdef MADV_REMOVE
  // MADV_REMOVE deletes any backing storage for tmpfs or anonymous shared
  // memory.
  do {
    ret = madvise(start, length, MADV_REMOVE);
  } while (ret == -1 && errno == EAGAIN);

  if (ret == 0) {
    return true;
  }
#endif

#ifdef MADV_FREE
  const bool do_madvfree = []() {
    switch (Parameters::madvise()) {
      case MadvisePreference::kFreeAndDontNeed:
      case MadvisePreference::kFreeOnly:
        return true;
      case MadvisePreference::kDontNeed:
      case MadvisePreference::kNever:
        return false;
    }

    ABSL_UNREACHABLE();
  }();

  if (do_madvfree) {
    do {
      ret = madvise(start, length, MADV_FREE);
    } while (ret == -1 && errno == EAGAIN);
  }
#endif
#ifdef MADV_DONTNEED
  const bool do_madvdontneed = []() {
    switch (Parameters::madvise()) {
      case MadvisePreference::kDontNeed:
      case MadvisePreference::kFreeAndDontNeed:
        return true;
      case MadvisePreference::kFreeOnly:
      case MadvisePreference::kNever:
        return false;
    }

    ABSL_UNREACHABLE();
  }();

  // MADV_DONTNEED drops page table info and any anonymous pages.
  if (do_madvdontneed) {
    do {
      ret = madvise(start, length, MADV_DONTNEED);
    } while (ret == -1 && errno == EAGAIN);
  }
#endif
  if (ret == 0) {
    return true;
  }

  return false;
}

}  // namespace tcmalloc::tcmalloc_internal::system_allocator_internal
GOOGLE_MALLOC_SECTION_END
