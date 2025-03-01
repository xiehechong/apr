/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apr.h"
#include <aclapi.h>
#include "apr_private.h"
#include "apr_arch_file_io.h"
#include "apr_file_io.h"
#include "apr_general.h"
#include "apr_strings.h"
#include "apr_errno.h"
#include "apr_time.h"
#include <sys/stat.h>
#include "apr_arch_atime.h"
#include "apr_arch_misc.h"

/* We have to assure that the file name contains no '*'s, or other
 * wildcards when using FindFirstFile to recover the true file name.
 */
static apr_status_t test_safe_name(const char *name)
{
    apr_status_t rv = APR_SUCCESS;

    /* Only accept ':' in the second position of the filename,
     * as the drive letter delimiter:
     */
    if (apr_isalpha(*name) && (name[1] == ':')) {
        name += 2;
    }
    while (*name) {
        if (!IS_FNCHAR(*name) && (*name != '\\') && (*name != '/')) {
            if (*name == '?' || *name == '*')
                rv = APR_EPATHWILD;
            else
                return APR_EBADPATH;
        }
        ++name;
    }
    return rv;
}

static apr_status_t free_localheap(void *heap) {
    LocalFree(heap);
    return APR_SUCCESS;
}

static apr_gid_t worldid = NULL;

static void free_world(void)
{
    if (worldid) {
        FreeSid(worldid);
        worldid = NULL;
    }
}

/* Left bit shifts from World scope to given scope */
typedef enum prot_scope_e {
    prot_scope_world = 0,
    prot_scope_group = 4,
    prot_scope_user =  8
} prot_scope_e;

static apr_fileperms_t convert_prot(ACCESS_MASK acc, prot_scope_e scope)
{
    /* These choices are based on the single filesystem bit that controls
     * the given behavior.  They are -not- recommended for any set protection
     * function, such a function should -set- use GENERIC_READ/WRITE/EXECUTE
     */
    apr_fileperms_t prot = 0;
    if (acc & FILE_EXECUTE)
        prot |= APR_FPROT_WEXECUTE;
    if (acc & FILE_WRITE_DATA)
        prot |= APR_FPROT_WWRITE;
    if (acc & FILE_READ_DATA)
        prot |= APR_FPROT_WREAD;
    return (prot << scope);
}

static void resolve_prot(apr_finfo_t *finfo, apr_int32_t wanted, PACL dacl)
{
    TRUSTEE_W ident = {NULL, NO_MULTIPLE_TRUSTEE, TRUSTEE_IS_SID};
    ACCESS_MASK acc;
    /*
     * This function is only invoked for WinNT, 
     * there is no reason for os_level testing here.
     */
    if ((wanted & APR_FINFO_WPROT) && !worldid) {
        SID_IDENTIFIER_AUTHORITY SIDAuth = {SECURITY_WORLD_SID_AUTHORITY};
        if (AllocateAndInitializeSid(&SIDAuth, 1, SECURITY_WORLD_RID,
                                     0, 0, 0, 0, 0, 0, 0, &worldid))
            atexit(free_world);
        else
            worldid = NULL;
    }
    if ((wanted & APR_FINFO_UPROT) && (finfo->valid & APR_FINFO_USER)) {
        ident.TrusteeType = TRUSTEE_IS_USER;
        ident.ptstrName = finfo->user;
        /* GetEffectiveRightsFromAcl isn't supported under Win9x,
         * which shouldn't come as a surprize.  Since we are passing
         * TRUSTEE_IS_SID, always skip the A->W layer.
         */
        if (GetEffectiveRightsFromAclW(dacl, &ident, &acc) == ERROR_SUCCESS) {
            finfo->protection |= convert_prot(acc, prot_scope_user);
            finfo->valid |= APR_FINFO_UPROT;
        }
    }
    /* Windows NT: did not return group rights.
     * Windows 2000 returns group rights information.
     * Since WinNT kernels don't follow the unix model of 
     * group associations, this all all pretty mute.
     */
    if ((wanted & APR_FINFO_GPROT) && (finfo->valid & APR_FINFO_GROUP)) {
        ident.TrusteeType = TRUSTEE_IS_GROUP;
        ident.ptstrName = finfo->group;
        if (GetEffectiveRightsFromAclW(dacl, &ident, &acc) == ERROR_SUCCESS) {
            finfo->protection |= convert_prot(acc, prot_scope_group);
            finfo->valid |= APR_FINFO_GPROT;
        }
    }
    if ((wanted & APR_FINFO_WPROT) && (worldid)) {
        ident.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ident.ptstrName = worldid;
        if (GetEffectiveRightsFromAclW(dacl, &ident, &acc) == ERROR_SUCCESS) {
            finfo->protection |= convert_prot(acc, prot_scope_world);
            finfo->valid |= APR_FINFO_WPROT;
        }
    }
}

static apr_status_t resolve_ident(apr_finfo_t *finfo, const char *fname,
                                  apr_int32_t wanted, apr_pool_t *pool)
{
    apr_file_t *thefile = NULL;
    apr_status_t rv;
    /* 
     * NT5 (W2K) only supports symlinks in the same manner as mount points.
     * This code should eventually take that into account, for now treat
     * every reparse point as a symlink...
     *
     * We must open the file with READ_CONTROL if we plan to retrieve the
     * user, group or permissions.
     */
    
    if ((rv = apr_file_open(&thefile, fname, APR_OPENINFO
                          | ((wanted & APR_FINFO_LINK) ? APR_OPENLINK : 0)
                          | ((wanted & (APR_FINFO_PROT | APR_FINFO_OWNER))
                                ? APR_READCONTROL : 0),
                            APR_FPROT_OS_DEFAULT, pool)) == APR_SUCCESS) {
        rv = apr_file_info_get(finfo, wanted, thefile);
        finfo->filehand = NULL;
        apr_file_close(thefile);
    }
    else if (APR_STATUS_IS_EACCES(rv) && (wanted & (APR_FINFO_PROT 
                                                  | APR_FINFO_OWNER))) {
        /* We have a backup plan.  Perhaps we couldn't grab READ_CONTROL?
         * proceed without asking for that permission...
         */
        if ((rv = apr_file_open(&thefile, fname, APR_OPENINFO
                              | ((wanted & APR_FINFO_LINK) ? APR_OPENLINK : 0),
                                APR_FPROT_OS_DEFAULT, pool)) == APR_SUCCESS) {
            rv = apr_file_info_get(finfo, wanted & ~(APR_FINFO_PROT 
                                                 | APR_FINFO_OWNER),
                                 thefile);
            finfo->filehand = NULL;
            apr_file_close(thefile);
        }
    }

    if (rv != APR_SUCCESS && rv != APR_INCOMPLETE)
        return (rv);

    /* We picked up this case above and had opened the link's properties */
    if (wanted & APR_FINFO_LINK)
        finfo->valid |= APR_FINFO_LINK;

    return rv;
}

static apr_status_t guess_protection_bits(apr_finfo_t *finfo,
                                          apr_int32_t wanted)
{
    /* Read, write execute for owner.  In the Win9x environment, any
     * readable file is executable (well, not entirely 100% true, but
     * still looking for some cheap logic that would help us here.)
     * The same holds on NT if a file doesn't have a DACL (e.g., on FAT)
     */
    if (finfo->protection & APR_FREADONLY) {
        finfo->protection |= APR_FPROT_WREAD | APR_FPROT_WEXECUTE;
    }
    else {
        finfo->protection |= APR_FPROT_WREAD | APR_FPROT_WEXECUTE | APR_FPROT_WWRITE;
    }
    finfo->protection |= (finfo->protection << prot_scope_group)
                       | (finfo->protection << prot_scope_user);

    finfo->valid |= APR_FINFO_UPROT | APR_FINFO_GPROT | APR_FINFO_WPROT;

    return ((wanted & ~finfo->valid) ? APR_INCOMPLETE : APR_SUCCESS);
}

static int reparse_point_is_link(WIN32_FILE_ATTRIBUTE_DATA *wininfo,
    int finddata, const char *fname)
{
    int tag = 0;

    if (!(wininfo->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        return 0;
    }

    if (finddata)
    {
        tag = ((WIN32_FIND_DATAW*)wininfo)->dwReserved0;
    }
    else
    {
        apr_wchar_t wfname[APR_PATH_MAX];
        HANDLE hFind;
        WIN32_FIND_DATAW fd;

        if (test_safe_name(fname) != APR_SUCCESS) {
            return 0;
        }

        if (utf8_to_unicode_path(wfname, APR_PATH_MAX, fname) != APR_SUCCESS) {
            return 0;
        }

        hFind = FindFirstFileExW(wfname, FindExInfoBasic, &fd, FindExSearchNameMatch,
                                 NULL, 0);
        if (hFind == INVALID_HANDLE_VALUE) {
            return 0;
        }

        FindClose(hFind);

        tag = fd.dwReserved0;
    }

    // Test "Name surrogate bit" to detect any kind of symbolic link
    // See https://docs.microsoft.com/en-us/windows/desktop/fileio/reparse-point-tags
    return IsReparseTagNameSurrogate(tag);
}

apr_status_t more_finfo(apr_finfo_t *finfo, const void *ufile, 
                        apr_int32_t wanted, int whatfile)
{
    PSID user = NULL, grp = NULL;
    PACL dacl = NULL;
    apr_status_t rv;

    if (apr_os_level < APR_WIN_NT)
        return guess_protection_bits(finfo, wanted);

    if (wanted & (APR_FINFO_PROT | APR_FINFO_OWNER))
    {
        /* On NT this request is incredibly expensive, but accurate.
         * Since the WinNT-only functions below are protected by the
         * (apr_os_level < APR_WIN_NT) case above, we need no extra
         * tests, but remember GetNamedSecurityInfo & GetSecurityInfo
         * are not supported on 9x.
         */
        SECURITY_INFORMATION sinf = 0;
        PSECURITY_DESCRIPTOR pdesc = NULL;
        if (wanted & (APR_FINFO_USER | APR_FINFO_UPROT))
            sinf |= OWNER_SECURITY_INFORMATION;
        if (wanted & (APR_FINFO_GROUP | APR_FINFO_GPROT))
            sinf |= GROUP_SECURITY_INFORMATION;
        if (wanted & APR_FINFO_PROT)
            sinf |= DACL_SECURITY_INFORMATION;
        if (whatfile == MORE_OF_WFSPEC) {
            apr_wchar_t *wfile = (apr_wchar_t*) ufile;
            int fix = 0;
            if (wcsncmp(wfile, L"\\\\?\\", 4) == 0) {
                fix = 4;
                if (wcsncmp(wfile + fix, L"UNC\\", 4) == 0)
                    wfile[6] = L'\\', fix = 6;
            }
            rv = GetNamedSecurityInfoW(wfile + fix, 
                                 SE_FILE_OBJECT, sinf,
                                 ((wanted & (APR_FINFO_USER | APR_FINFO_UPROT)) ? &user : NULL),
                                 ((wanted & (APR_FINFO_GROUP | APR_FINFO_GPROT)) ? &grp : NULL),
                                 ((wanted & APR_FINFO_PROT) ? &dacl : NULL),
                                 NULL, &pdesc);
            if (fix == 6)
                wfile[6] = L'C';
        }
        else if (whatfile == MORE_OF_HANDLE)
            rv = GetSecurityInfo((HANDLE)ufile, 
                                 SE_FILE_OBJECT, sinf,
                                 ((wanted & (APR_FINFO_USER | APR_FINFO_UPROT)) ? &user : NULL),
                                 ((wanted & (APR_FINFO_GROUP | APR_FINFO_GPROT)) ? &grp : NULL),
                                 ((wanted & APR_FINFO_PROT) ? &dacl : NULL),
                                 NULL, &pdesc);
        else
            return APR_INCOMPLETE; /* should not occur */
        if (rv == ERROR_SUCCESS)
            apr_pool_cleanup_register(finfo->pool, pdesc, free_localheap, 
                                 apr_pool_cleanup_null);
        else
            user = grp = dacl = NULL;

        if (user) {
            finfo->user = user;
            finfo->valid |= APR_FINFO_USER;
        }

        if (grp) {
            finfo->group = grp;
            finfo->valid |= APR_FINFO_GROUP;
        }

        if (dacl) {
            /* Retrieved the discresionary access list */
            resolve_prot(finfo, wanted, dacl);
        }
        else if (wanted & APR_FINFO_PROT)
            guess_protection_bits(finfo, wanted);
    }

    if ((wanted & APR_FINFO_CSIZE) && (finfo->filetype == APR_REG))
    {
        if (whatfile == MORE_OF_HANDLE) {
            FILE_ALLOCATION_INFO fi;
            if (GetFileInformationByHandleEx((HANDLE)ufile, FileAllocationInfo, &fi, sizeof(fi))) {
                finfo->csize = fi.AllocationSize.QuadPart;
                finfo->valid |= APR_FINFO_CSIZE;
            }
        }
        else {
            DWORD sizelo, sizehi;

            SetLastError(NO_ERROR);
            if (whatfile == MORE_OF_WFSPEC)
                sizelo = GetCompressedFileSizeW((apr_wchar_t*)ufile, &sizehi);
            else
                return APR_EGENERAL; /* should not occur */
        
            if (sizelo != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
#if APR_HAS_LARGE_FILES
                finfo->csize =  (apr_off_t)sizelo
                             | ((apr_off_t)sizehi << 32);
#else
                finfo->csize = (apr_off_t)sizelo;
                if (finfo->csize < 0 || sizehi)
                    finfo->csize = 0x7fffffff;
#endif
                finfo->valid |= APR_FINFO_CSIZE;
            }
        }
    }
    return ((wanted & ~finfo->valid) ? APR_INCOMPLETE : APR_SUCCESS);
}


/* This generic fillin depends upon byhandle to be passed as 0 when
 * a WIN32_FILE_ATTRIBUTE_DATA or either WIN32_FIND_DATA [A or W] is
 * passed for wininfo.  When the BY_HANDLE_FILE_INFORMATION structure
 * is passed for wininfo, byhandle is passed as 1 to offset the one
 * dword discrepancy in offset of the High/Low size structure members.
 *
 * The generic fillin returns 1 if the caller should further inquire
 * if this is a CHR filetype.  If it's reasonably certain it can't be,
 * then the function returns 0.
 */
int fillin_fileinfo(apr_finfo_t *finfo, 
                    WIN32_FILE_ATTRIBUTE_DATA *wininfo, 
                    int byhandle,
                    int finddata,
                    const char *fname,
                    apr_int32_t wanted)
{
    DWORD *sizes = &wininfo->nFileSizeHigh + byhandle;
    int warn = 0;

    memset(finfo, '\0', sizeof(*finfo));

    FileTimeToAprTime(&finfo->atime, &wininfo->ftLastAccessTime);
    FileTimeToAprTime(&finfo->ctime, &wininfo->ftCreationTime);
    FileTimeToAprTime(&finfo->mtime, &wininfo->ftLastWriteTime);

#if APR_HAS_LARGE_FILES
    finfo->size =  (apr_off_t)sizes[1]
                | ((apr_off_t)sizes[0] << 32);
#else
    finfo->size = (apr_off_t)sizes[1];
    if (finfo->size < 0 || sizes[0])
        finfo->size = 0x7fffffff;
#endif

    if (wanted & APR_FINFO_LINK &&
        reparse_point_is_link(wininfo, finddata, fname)) {
        finfo->filetype = APR_LNK;
    }
    else if (wininfo->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        finfo->filetype = APR_DIR;
    }
    else if (wininfo->dwFileAttributes & FILE_ATTRIBUTE_DEVICE) {
        /* Warning: This test only succeeds on Win9x, on NT these files
         * (con, aux, nul, lpt#, com# etc) escape early detection!
         */
        finfo->filetype = APR_CHR;
    }
    else {
        /* Warning: Short of opening the handle to the file, the 'FileType'
         * appears to be unknowable (in any trustworthy or consistent sense)
         * on WinNT/2K as far as PIPE, CHR, etc are concerned.
         */
        if (!wininfo->ftLastWriteTime.dwLowDateTime 
                && !wininfo->ftLastWriteTime.dwHighDateTime 
                && !finfo->size)
            warn = 1;
        finfo->filetype = APR_REG;
    }

    /* The following flags are [for this moment] private to Win32.
     * That's the only excuse for not toggling valid bits to reflect them.
     */
    if (wininfo->dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        finfo->protection = APR_FREADONLY;
    
    finfo->valid = APR_FINFO_ATIME | APR_FINFO_CTIME | APR_FINFO_MTIME
                 | APR_FINFO_SIZE  | APR_FINFO_TYPE;   /* == APR_FINFO_MIN */

    /* Only byhandle optionally tests link targets, so tell that caller
     * what it wants to hear, otherwise the byattributes is never
     * reporting anything but the link.
     */
    if (!byhandle || (wanted & APR_FINFO_LINK))
        finfo->valid |= APR_FINFO_LINK;
    return warn;
}


APR_DECLARE(apr_status_t) apr_file_info_get(apr_finfo_t *finfo, apr_int32_t wanted,
                                            apr_file_t *thefile)
{
    BY_HANDLE_FILE_INFORMATION FileInfo;

    if (thefile->buffered) {
        /* XXX: flush here is not mutex protected */
        apr_status_t rv = apr_file_flush(thefile);
        if (rv != APR_SUCCESS)
            return rv;
    }

    /* GetFileInformationByHandle() is implemented via two syscalls:
     * QueryInformationVolume and QueryAllInformationFile. Use cheaper
     * GetFileSizeEx() API if we only need to get the file size. */
    if (wanted == APR_FINFO_SIZE) {
       LARGE_INTEGER size;

       if (!GetFileSizeEx(thefile->filehand, &size)) {
          return apr_get_os_error();
       }

       finfo->pool = thefile->pool;
       finfo->fname = thefile->fname;
       finfo->size = size.QuadPart;
       finfo->valid = APR_FINFO_SIZE;

       return APR_SUCCESS;
    }

    if (!GetFileInformationByHandle(thefile->filehand, &FileInfo)) {
        return apr_get_os_error();
    }

    memset(finfo, '\0', sizeof(*finfo));

    FileTimeToAprTime(&finfo->atime, &FileInfo.ftLastAccessTime);
    FileTimeToAprTime(&finfo->ctime, &FileInfo.ftCreationTime);
    FileTimeToAprTime(&finfo->mtime, &FileInfo.ftLastWriteTime);

#if APR_HAS_LARGE_FILES
    finfo->size = (apr_off_t)FileInfo.nFileSizeLow
        | ((apr_off_t)FileInfo.nFileSizeHigh << 32);
#else
    finfo->size = (apr_off_t)FileInfo.nFileSizeLow;
    if (finfo->size < 0 || FileInfo.nFileSizeHigh)
        finfo->size = 0x7fffffff;
#endif

    if (wanted & APR_FINFO_LINK &&
        reparse_point_is_link((WIN32_FILE_ATTRIBUTE_DATA *) &FileInfo, 0, thefile->fname)) {
        finfo->filetype = APR_LNK;
    }
    else if (FileInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        finfo->filetype = APR_DIR;
    }
    else if (FileInfo.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) {
        /* Warning: This test only succeeds on Win9x, on NT these files
         * (con, aux, nul, lpt#, com# etc) escape early detection!
         */
        finfo->filetype = APR_CHR;
    }
    else {
        /* Warning: Short of opening the handle to the file, the 'FileType'
         * appears to be unknowable (in any trustworthy or consistent sense)
         * on WinNT/2K as far as PIPE, CHR, etc are concerned.
         */
        finfo->filetype = APR_REG;
    }

    /* The following flags are [for this moment] private to Win32.
     * That's the only excuse for not toggling valid bits to reflect them.
     */
    if (FileInfo.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        finfo->protection = APR_FREADONLY;

    finfo->valid = APR_FINFO_ATIME | APR_FINFO_CTIME | APR_FINFO_MTIME
        | APR_FINFO_SIZE | APR_FINFO_TYPE;   /* == APR_FINFO_MIN */

    /* Only byhandle optionally tests link targets, so tell that caller
     * what it wants to hear, otherwise the byattributes is never
     * reporting anything but the link.
     */
    if (wanted & APR_FINFO_LINK)
        finfo->valid |= APR_FINFO_LINK;

    if (finfo->filetype == APR_REG)
    {
        /* Go the extra mile to be -certain- that we have a real, regular
         * file, since the attribute bits aren't a certain thing.  Even
         * though fillin should have hinted if we *must* do this, we
         * don't need to take chances while the handle is already open.
         */
        DWORD FileType;
        if ((FileType = GetFileType(thefile->filehand))) {
            if (FileType == FILE_TYPE_CHAR) {
                finfo->filetype = APR_CHR;
            }
            else if (FileType == FILE_TYPE_PIPE) {
                finfo->filetype = APR_PIPE;
            }
            /* Otherwise leave the original conclusion alone 
             */
        }
    }

    finfo->pool = thefile->pool;

    /* ### The finfo lifetime may exceed the lifetime of thefile->pool
     * but finfo's aren't managed in pools, so where on earth would
     * we pstrdup the fname into???
     */
    finfo->fname = thefile->fname;
 
    /* Extra goodies known only by GetFileInformationByHandle() */
    finfo->inode  =  (apr_ino_t)FileInfo.nFileIndexLow
                  | ((apr_ino_t)FileInfo.nFileIndexHigh << 32);
    finfo->device = FileInfo.dwVolumeSerialNumber;
    finfo->nlink  = FileInfo.nNumberOfLinks;

    finfo->valid |= APR_FINFO_IDENT | APR_FINFO_NLINK;

    /* If we still want something more (besides the name) go get it! 
     */
    if ((wanted &= ~finfo->valid) & ~APR_FINFO_NAME) {
        return more_finfo(finfo, thefile->filehand, wanted, MORE_OF_HANDLE);
    }

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_file_perms_set(const char *fname,
                                           apr_fileperms_t perms)
{
    return APR_ENOTIMPL;
}

APR_DECLARE(apr_status_t) apr_stat(apr_finfo_t *finfo, const char *fname,
                                   apr_int32_t wanted, apr_pool_t *pool)
{
    /* XXX: is constant - needs testing - which requires a lighter-weight root test fn */
    int isroot = 0;
    apr_status_t ident_rv = 0;
    apr_status_t rv;
    apr_wchar_t wfname[APR_PATH_MAX];
    char *filename = NULL;
    /* These all share a common subset of this structure */
    union {
        WIN32_FIND_DATAW w;
        WIN32_FILE_ATTRIBUTE_DATA i;
    } FileInfo;
    int finddata = 0;
    
    /* Catch fname length == MAX_PATH since GetFileAttributesEx fails 
     * with PATH_NOT_FOUND.  We would rather indicate length error than 
     * 'not found'
     */        
    if (strlen(fname) >= APR_PATH_MAX) {
        return APR_ENAMETOOLONG;
    }

    if ((wanted & (APR_FINFO_IDENT | APR_FINFO_NLINK)) 
           || (~wanted & APR_FINFO_LINK)) {
        /* FindFirstFile and GetFileAttributesEx can't figure the inode,
         * device or number of links, so we need to resolve with an open 
         * file handle.  If the user has asked for these fields, fall over 
         * to the get file info by handle method.  If we fail, or the user
         * also asks for the file name, continue by our usual means.
         *
         * We also must use this method for a 'true' stat, that resolves
         * a symlink (NTFS Junction) target.  This is because all fileinfo
         * on a Junction always returns the junction, opening the target
         * is the only way to resolve the target's attributes.
         */
        if ((ident_rv = resolve_ident(finfo, fname, wanted, pool)) 
                == APR_SUCCESS)
            return ident_rv;
        else if (ident_rv == APR_INCOMPLETE)
            wanted &= ~finfo->valid;
    }

    if ((rv = utf8_to_unicode_path(wfname, sizeof(wfname) 
                                        / sizeof(apr_wchar_t), fname)))
        return rv;
    if (!(wanted & APR_FINFO_NAME)) {
        if (!GetFileAttributesExW(wfname, GetFileExInfoStandard, 
                                  &FileInfo.i))
            return apr_get_os_error();
    }
    else {
        /* Guard against bogus wildcards and retrieve by name
         * since we want the true name, and set aside a long
         * enough string to handle the longest file name.
         */
        char tmpname[APR_FILE_MAX * 3 + 1];
        HANDLE hFind;
        if ((rv = test_safe_name(fname)) != APR_SUCCESS) {
            return rv;
        }
        hFind = FindFirstFileExW(wfname, FindExInfoBasic, &FileInfo.w,
                                 FindExSearchNameMatch, NULL, 0);
        if (hFind == INVALID_HANDLE_VALUE)
            return apr_get_os_error();
        FindClose(hFind);
        finddata = 1;
        if (unicode_to_utf8_path(tmpname, sizeof(tmpname),
                                 FileInfo.w.cFileName)) {
            return APR_ENAMETOOLONG;
        }
        filename = apr_pstrdup(pool, tmpname);
    }

    if (ident_rv != APR_INCOMPLETE) {
        if (fillin_fileinfo(finfo, (WIN32_FILE_ATTRIBUTE_DATA *) &FileInfo, 
                            0, finddata, fname, wanted))
        {
            /* Go the extra mile to assure we have a file.  WinNT/2000 seems
             * to reliably translate char devices to the path '\\.\device'
             * so go ask for the full path.
             */
            apr_wchar_t tmpname[APR_FILE_MAX];
            apr_wchar_t *tmpoff = NULL;
            if (GetFullPathNameW(wfname, sizeof(tmpname) / sizeof(apr_wchar_t),
                                 tmpname, &tmpoff))
            {
                if (!wcsncmp(tmpname, L"\\\\.\\", 4)) {
                    if (tmpoff == tmpname + 4) {
                        finfo->filetype = APR_CHR;
                    }
                    /* For WHATEVER reason, CHR devices such as \\.\con 
                     * or \\.\lpt1 *may*not* update tmpoff; in fact the
                     * resulting tmpoff is set to NULL.  Guard against 
                     * either case.
                     *
                     * This code is identical for wide and narrow chars...
                     */
                    else if (!tmpoff) {
                        tmpoff = tmpname + 4;
                        while (*tmpoff) {
                            if (*tmpoff == '\\' || *tmpoff == '/') {
                                break;
                            }
                            ++tmpoff;
                        }
                        if (!*tmpoff) {
                            finfo->filetype = APR_CHR;
                        }
                    }
                }
            }
            else {
                finfo->valid &= ~APR_FINFO_TYPE;
            }
        }
        finfo->pool = pool;
    }

    if (filename && !isroot) {
        finfo->name = filename;
        finfo->valid |= APR_FINFO_NAME;
    }

    if (wanted &= ~finfo->valid) {
        /* Caller wants more than APR_FINFO_MIN | APR_FINFO_NAME */
        return more_finfo(finfo, wfname, wanted, MORE_OF_WFSPEC);
    }

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_file_attrs_set(const char *fname,
                                             apr_fileattrs_t attributes,
                                             apr_fileattrs_t attr_mask,
                                             apr_pool_t *pool)
{
    DWORD old_flags;
    DWORD new_flags;
    apr_status_t rv;
    apr_wchar_t wfname[APR_PATH_MAX];

    /* Don't do anything if we can't handle the requested attributes */
    if (!(attr_mask & (APR_FILE_ATTR_READONLY
                       | APR_FILE_ATTR_HIDDEN)))
        return APR_SUCCESS;

    if ((rv = utf8_to_unicode_path(wfname,
                                    sizeof(wfname) / sizeof(wfname[0]),
                                    fname)))
        return rv;

    old_flags = GetFileAttributesW(wfname);
    if (old_flags == 0xFFFFFFFF)
        return apr_get_os_error();

    new_flags = old_flags;
    if (attr_mask & APR_FILE_ATTR_READONLY)
    {
        if (attributes & APR_FILE_ATTR_READONLY)
            new_flags |= FILE_ATTRIBUTE_READONLY;
        else
            new_flags &= ~FILE_ATTRIBUTE_READONLY;
    }

    if (attr_mask & APR_FILE_ATTR_HIDDEN)
    {
        if (attributes & APR_FILE_ATTR_HIDDEN)
            new_flags |= FILE_ATTRIBUTE_HIDDEN;
        else
            new_flags &= ~FILE_ATTRIBUTE_HIDDEN;
    }

    /* Don't do anything if we are not going to change attributes. */
    if (new_flags == old_flags) {
        return APR_SUCCESS;
    }

    if (!SetFileAttributesW(wfname, new_flags)) {
        return apr_get_os_error();
    }

    return APR_SUCCESS;
}


APR_DECLARE(apr_status_t) apr_file_mtime_set(const char *fname,
                                             apr_time_t mtime,
                                             apr_pool_t *pool)
{
    apr_file_t *thefile;
    apr_status_t rv;

    rv = apr_file_open(&thefile, fname,
                       APR_FOPEN_READ | APR_WRITEATTRS,
                       APR_FPROT_OS_DEFAULT, pool);
    if (!rv)
    {
        FILETIME file_mtime;

        AprTimeToFileTime(&file_mtime, mtime);
        if (!SetFileTime(thefile->filehand, NULL, NULL, &file_mtime))
            rv = apr_get_os_error();

        apr_file_close(thefile);
    }

    return rv;
}
