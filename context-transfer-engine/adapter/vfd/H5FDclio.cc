/*
 * Copyright (c) 2024, Gnosis Research Center, Illinois Institute of Technology
 * All rights reserved.
 *
 * This file is part of IOWarp Core.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Programmer:  Kimmy Mu
 *              March 2021
 *
 * Purpose: An HDF5 Virtual File Driver that writes every byte through to an
 *          authoritative on-disk native HDF5 file (so standard tools read it
 *          live), while opening a CLIO CTE handle alongside as groundwork for a
 *          future read/tiering cache.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* HDF5 header for dynamic plugin loading */
#include "H5FDclio.h" /* Clio file driver     */
#include "H5PLextern.h"
#include "adapter/cfs/cfs_io.h"
#include "clio_cte/core/core_client.h"
#include <clio_ctp/util/logging.h>

/* The driver identification number, initialized at runtime */
static hid_t H5FD_CLIO_g = H5I_INVALID_HID;

/* Identifiers for HDF5's error API */
hid_t H5FDclio_err_stack_g = H5I_INVALID_HID;
hid_t H5FDclio_err_class_g = H5I_INVALID_HID;

/* POSIX I/O mode used as the third parameter to open/_open
 * when creating a new file (O_CREAT is set). */
#if defined(H5_HAVE_WIN32_API)
#define H5FD_CLIO_POSIX_CREATE_MODE_RW (_S_IREAD | _S_IWRITE)
#else
#define H5FD_CLIO_POSIX_CREATE_MODE_RW 0666
#endif

#define MAXADDR (((haddr_t)1 << (8 * sizeof(off_t) - 1)) - 1)
#define SUCCEED 0
#define FAIL (-1)

#ifdef __cplusplus
extern "C" {
#endif

/* The description of a file belonging to this driver. */
typedef struct H5FD_clio_t {
  H5FD_t pub;      /* public stuff, must be first           */
  haddr_t eoa;     /* end of allocated region               */
  haddr_t eof;     /* end of file; current file size        */
  int fd;          /* CTE cache handle (-1 if none this session) */
  int posix_fd;    /* authoritative on-disk native file fd  */
  char *filename_; /* the name of the file (NULL if empty)  */
  unsigned flags;  /* the flags passed from H5Fcreate/H5Fopen */
} H5FD_clio_t;

/* Prototypes */
static herr_t H5FD__clio_term(void);
static H5FD_t *H5FD__clio_open(const char *name, unsigned flags,
                                 hid_t fapl_id, haddr_t maxaddr);
static herr_t H5FD__clio_close(H5FD_t *_file);
static int H5FD__clio_cmp(const H5FD_t *_f1, const H5FD_t *_f2);
static herr_t H5FD__clio_query(const H5FD_t *_f1, unsigned long *flags);
static haddr_t H5FD__clio_get_eoa(const H5FD_t *_file, H5FD_mem_t type);
static herr_t H5FD__clio_set_eoa(H5FD_t *_file, H5FD_mem_t type,
                                   haddr_t addr);
static haddr_t H5FD__clio_get_eof(const H5FD_t *_file, H5FD_mem_t type);
static herr_t H5FD__clio_read(H5FD_t *_file, H5FD_mem_t type, hid_t fapl_id,
                                haddr_t addr, size_t size, void *buf);
static herr_t H5FD__clio_write(H5FD_t *_file, H5FD_mem_t type, hid_t fapl_id,
                                 haddr_t addr, size_t size, const void *buf);

static const H5FD_class_t H5FD_clio_g = {
    H5FD_CLASS_VERSION,   /* struct version       */
    H5FD_CLIO_VALUE,   /* value                */
    H5FD_CLIO_NAME,    /* name                 */
    MAXADDR,              /* maxaddr              */
    H5F_CLOSE_STRONG,     /* fc_degree            */
    H5FD__clio_term,    /* terminate            */
    NULL,                 /* sb_size              */
    NULL,                 /* sb_encode            */
    NULL,                 /* sb_decode            */
    0,                    /* fapl_size            */
    NULL,                 /* fapl_get             */
    NULL,                 /* fapl_copy            */
    NULL,                 /* fapl_free            */
    0,                    /* dxpl_size            */
    NULL,                 /* dxpl_copy            */
    NULL,                 /* dxpl_free            */
    H5FD__clio_open,    /* open                 */
    H5FD__clio_close,   /* close                */
    H5FD__clio_cmp,     /* cmp                  */
    H5FD__clio_query,   /* query                */
    NULL,                 /* get_type_map         */
    NULL,                 /* alloc                */
    NULL,                 /* free                 */
    H5FD__clio_get_eoa, /* get_eoa              */
    H5FD__clio_set_eoa, /* set_eoa              */
    H5FD__clio_get_eof, /* get_eof              */
    NULL,                 /* get_handle           */
    H5FD__clio_read,    /* read                 */
    H5FD__clio_write,   /* write                */
    NULL,                 /* read_vector          */
    NULL,                 /* write_vector         */
    NULL,                 /* read_selection       */
    NULL,                 /* write_selection      */
    NULL,                 /* flush                */
    NULL,                 /* truncate             */
    NULL,                 /* lock                 */
    NULL,                 /* unlock               */
    NULL,                 /* del                  */
    NULL,                 /* ctl                  */
    H5FD_FLMAP_DICHOTOMY  /* fl_map               */
};

/*-------------------------------------------------------------------------
 * Function:    H5FD_clio_init
 *
 * Purpose:     Initialize this driver by registering the driver with the
 *              library.
 *
 * Return:      Success:    The driver ID for the clio driver
 *              Failure:    H5I_INVALID_HID
 *
 *-------------------------------------------------------------------------
 */
hid_t H5FD_clio_init(void) {
  hid_t ret_value = H5I_INVALID_HID; /* Return value */

  if (H5I_VFL != H5Iget_type(H5FD_CLIO_g)) {
    H5FD_CLIO_g = H5FDregister(&H5FD_clio_g);
  }

  /* Set return value */
  ret_value = H5FD_CLIO_g;
  return ret_value;
} /* end H5FD_clio_init() */

/*---------------------------------------------------------------------------
 * Function:    H5FD__clio_term
 *
 * Purpose:     Shut down the VFD
 *
 * Returns:     SUCCEED (Can't fail)
 *
 *---------------------------------------------------------------------------
 */
static herr_t H5FD__clio_term(void) {
  herr_t ret_value = SUCCEED;

  /* Unregister from HDF5 error API */
  if (H5FDclio_err_class_g >= 0) {
    if (H5Eunregister_class(H5FDclio_err_class_g) < 0) {
      // TODO(llogan)
    }

    /* Destroy the error stack */
    if (H5Eclose_stack(H5FDclio_err_stack_g) < 0) {
      // TODO(llogan)
    } /* end if */

    H5FDclio_err_stack_g = H5I_INVALID_HID;
    H5FDclio_err_class_g = H5I_INVALID_HID;
  }

  /* Reset VFL ID */
  H5FD_CLIO_g = H5I_INVALID_HID;

  return ret_value;
} /* end H5FD__clio_term() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_open
 *
 * Purpose:     Create and/or open a file. The authoritative store is a real
 *              on-disk native HDF5 file; a CTE cache handle is opened alongside
 *              as groundwork for a future read/tiering cache.
 *
 * Return:      Success:    A pointer to a new file data structure.
 *              Failure:    NULL
 *
 *-------------------------------------------------------------------------
 */
static H5FD_t *H5FD__clio_open(const char *name, unsigned flags,
                                 hid_t fapl_id, haddr_t maxaddr) {
  (void)fapl_id;
  (void)maxaddr;
  clio::cte::core::CLIO_CTE_CLIENT_INIT();

  /* Build the open flags */
  int o_flags = (H5F_ACC_RDWR & flags) ? O_RDWR : O_RDONLY;
  if (H5F_ACC_TRUNC & flags) {
    o_flags |= O_TRUNC;
  }
  if (H5F_ACC_CREAT & flags) {
    o_flags |= O_CREAT;
  }
  if (H5F_ACC_EXCL & flags) {
    o_flags |= O_EXCL;
  }

  // The AUTHORITATIVE store is a real on-disk native HDF5 file at the stripped
  // path (clio::/tmp/foo.h5 -> /tmp/foo.h5), so standard tools (h5dump/h5ls)
  // read it live.
  std::string native_path = clio::cae::StripClioPrefix(name);
  int posix_fd =
      open(native_path.c_str(), o_flags, H5FD_CLIO_POSIX_CREATE_MODE_RW);
  if (posix_fd < 0) {
    // Fail-closed: no authoritative file => the open fails. We do not proceed
    // with a cache-only file.
    return nullptr;
  }

  // FUTURE (CTE tiering): open a CTE/CFS handle for this file so writes can
  // populate a fast cache tier and reads can eventually be served from it.
  // Today the handle is populated on write but NOT yet served on reads, so the
  // open is best-effort: a cache-open failure must not sink the open (the
  // authoritative native file already succeeded) -- fd == -1 just means "no
  // cache this session".
  int fd = CLIO_CTE_CFS->Open(name, o_flags, H5FD_CLIO_POSIX_CREATE_MODE_RW);
  HLOG(kDebug, "");

  /* Create the new file struct */
  H5FD_clio_t *file = (H5FD_clio_t *)calloc(1, sizeof(H5FD_clio_t));
  if (file == NULL) {
    // Out of memory: release the handles we already opened instead of leaking
    // them (and dereferencing a NULL file).
    close(posix_fd);
    if (fd >= 0) {
      CLIO_CTE_CFS->Close(fd);
    }
    return nullptr;
  }

  /* Pack file */
  if (name && *name) {
    file->filename_ = strdup(name);
  }
  file->fd = fd;
  file->posix_fd = posix_fd;
  file->flags = flags;

  // EOF is the authoritative on-disk size (durable across reopen/append), not a
  // session-local counter or the cache's logical size.
  struct stat st;
  file->eof = (fstat(posix_fd, &st) == 0) ? (haddr_t)st.st_size : 0;

  return (H5FD_t *)file;
} /* end H5FD__clio_open() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_close
 *
 * Purpose:     Closes an HDF5 file.
 *
 * Return:      Success:    SUCCEED
 *              Failure:    FAIL, file not closed.
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5FD__clio_close(H5FD_t *_file) {
  H5FD_clio_t *file = (H5FD_clio_t *)_file;
  herr_t ret_value = SUCCEED; /* Return value */
  assert(file);
  // fsync + close the authoritative native file first -- a successful close is
  // a durability barrier (no pending dirty state), and the on-disk file is a
  // complete valid native HDF5 image afterward.
  if (file->posix_fd >= 0) {
    fsync(file->posix_fd);
    close(file->posix_fd);
  }
  // Release the CTE cache handle, if this session had one.
  if (file->fd >= 0) {
    CLIO_CTE_CFS->Close(file->fd);
    HLOG(kDebug, "");
  }
  if (file->filename_) {
    free(file->filename_);
  }
  free(file);
  return ret_value;
} /* end H5FD__clio_close() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_cmp
 *
 * Purpose:     Compares two files belonging to this driver using an arbitrary
 *              (but consistent) ordering.
 *
 * Return:      Success:    A value like strcmp()
 *              Failure:    never fails (arguments were checked by the
 *                          caller).
 *
 *-------------------------------------------------------------------------
 */
static int H5FD__clio_cmp(const H5FD_t *_f1, const H5FD_t *_f2) {
  const H5FD_clio_t *f1 = (const H5FD_clio_t *)_f1;
  const H5FD_clio_t *f2 = (const H5FD_clio_t *)_f2;
  // filename_ is only set for a non-empty name; guard against NULL so an
  // empty-name file cannot crash strcmp.
  const char *n1 = f1->filename_ ? f1->filename_ : "";
  const char *n2 = f2->filename_ ? f2->filename_ : "";
  return strcmp(n1, n2);
} /* end H5FD__clio_cmp() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_query
 *
 * Purpose:     Set the flags that this VFL driver is capable of supporting.
 *              (listed in H5FDpublic.h)
 *
 * Return:      SUCCEED (Can't fail)
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5FD__clio_query(const H5FD_t *_file,
                                 unsigned long *flags /* out */) {
  (void)_file;
  // No feature flags are advertised yet (a dedicated change will advertise the
  // sec2-compatible set). Returning 0 is conservative but disables HDF5's
  // metadata-aggregation / data-sieve optimizations.
  if (flags) {
    *flags = 0;
  }
  return SUCCEED;
} /* end H5FD__clio_query() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_get_eoa
 *
 * Purpose:     Gets the end-of-address marker for the file. The EOA marker
 *              is the first address past the last byte allocated in the
 *              format address space.
 *
 * Return:      The end-of-address marker.
 *
 *-------------------------------------------------------------------------
 */
static haddr_t H5FD__clio_get_eoa(const H5FD_t *_file, H5FD_mem_t type) {
  (void)type;
  const H5FD_clio_t *file = (const H5FD_clio_t *)_file;
  return file->eoa;
} /* end H5FD__clio_get_eoa() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_set_eoa
 *
 * Purpose:     Set the end-of-address marker for the file. This function is
 *              called shortly after an existing HDF5 file is opened in order
 *              to tell the driver where the end of the HDF5 data is located.
 *
 * Return:      SUCCEED (Can't fail)
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5FD__clio_set_eoa(H5FD_t *_file, H5FD_mem_t type,
                                   haddr_t addr) {
  (void)type;
  H5FD_clio_t *file = (H5FD_clio_t *)_file;
  file->eoa = addr;
  return SUCCEED;
} /* end H5FD__clio_set_eoa() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_get_eof
 *
 * Purpose:     Returns the end-of-file marker, which is the greater of
 *              either the filesystem end-of-file or the HDF5 end-of-address
 *              markers.
 *
 * Return:      End of file address, the first address past the end of the
 *              "file", either the filesystem file or the HDF5 file.
 *
 *-------------------------------------------------------------------------
 */
static haddr_t H5FD__clio_get_eof(const H5FD_t *_file, H5FD_mem_t type) {
  (void)type;
  const H5FD_clio_t *file = (const H5FD_clio_t *)_file;
  return file->eof;
} /* end H5FD__clio_get_eof() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_read
 *
 * Purpose:     Reads SIZE bytes of data from FILE beginning at address ADDR
 *              into buffer BUF. Reads come from the authoritative native file;
 *              a read of a region past the last byte ever written is
 *              zero-filled (HDF5 treats the file as a flat byte array).
 *
 * Return:      Success:    SUCCEED. Result is stored in caller-supplied
 *                          buffer BUF.
 *              Failure:    FAIL, Contents of buffer BUF are undefined.
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5FD__clio_read(H5FD_t *_file, H5FD_mem_t type, hid_t dxpl_id,
                                haddr_t addr, size_t size, void *buf) {
  (void)dxpl_id;
  (void)type;
  H5FD_clio_t *file = (H5FD_clio_t *)_file;

  // FUTURE (CTE read tier): consult the cache first and serve a hit from a fast
  // tier, falling back to the native file on a miss (and populating on the
  // way). This needs per-file tracking of which byte ranges are populated --
  // the CFS chimod zero-fills holes and reports them as a full read, so a naive
  // cache lookup could hand back stale zeros. Until that exists, reads come
  // straight from the authoritative native file so correctness never depends on
  // the cache.
  ssize_t count = pread(file->posix_fd, buf, size, static_cast<off_t>(addr));
  if (getenv("CLIO_VFD_DEBUG"))
    fprintf(stderr, "[vfd] READ  addr=%llu size=%llu got=%lld\n",
            (unsigned long long)addr, (unsigned long long)size,
            (long long)count);
  HLOG(kDebug, "");

  // Distinguish a genuine read error from EOF: a negative count is a failure
  // and must NOT be masked as zero data; a short read (0 <= count < size) is
  // EOF and the unread tail is zero-filled.
  if (count < 0) {
    return FAIL;
  }
  size_t got = static_cast<size_t>(count);
  if (got < size) {
    memset(static_cast<char *>(buf) + got, 0, size - got);
  }
  return SUCCEED;
} /* end H5FD__clio_read() */

/*-------------------------------------------------------------------------
 * Function:    H5FD__clio_write
 *
 * Purpose:     Writes SIZE bytes of data from buffer BUF at file address ADDR.
 *              The write is committed synchronously to the authoritative native
 *              file; a short/failed write is fail-closed.
 *
 * Return:      SUCCEED/FAIL
 *
 *-------------------------------------------------------------------------
 */
static herr_t H5FD__clio_write(H5FD_t *_file, H5FD_mem_t type, hid_t dxpl_id,
                                 haddr_t addr, size_t size, const void *buf) {
  (void)dxpl_id;
  (void)type;
  H5FD_clio_t *file = (H5FD_clio_t *)_file;

  // Commit point: write-through to the authoritative native file, synchronously.
  ssize_t count = pwrite(file->posix_fd, buf, size, static_cast<off_t>(addr));
  if (getenv("CLIO_VFD_DEBUG"))
    fprintf(stderr, "[vfd] WRITE addr=%llu size=%llu put=%lld\n",
            (unsigned long long)addr, (unsigned long long)size,
            (long long)count);
  if (count < 0 || static_cast<size_t>(count) < size) {
    return FAIL;
  }

  // FUTURE (CTE tiering): populate the cache tier with these bytes. This is
  // written today as groundwork but is NOT yet served on reads (see the read
  // callback), so it is best-effort -- its failure never fails the
  // authoritative write, and it is skipped when there is no cache handle.
  if (file->fd >= 0) {
    CLIO_CTE_CFS->Pwrite(file->fd, buf, size, static_cast<off_t>(addr));
    HLOG(kDebug, "");
  }

  // Track end-of-file so get_eof stays accurate within a session (HDF5 checks
  // it to validate file size).
  if ((haddr_t)(addr + size) > file->eof) {
    file->eof = (haddr_t)(addr + size);
  }
  return SUCCEED;
} /* end H5FD__clio_write() */

/*
 * Entry points for dynamic plugin loading.
 */
H5PL_type_t H5PLget_plugin_type(void) { return H5PL_TYPE_VFD; }

const void *H5PLget_plugin_info(void) { return &H5FD_clio_g; }

} // extern C
