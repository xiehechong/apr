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
#include "apr_arch_file_io.h"
#include "apr_file_io.h"
#include "apr_strings.h"
#include "apr_portable.h"
#include "apr_arch_atime.h"

#if APR_HAVE_ERRNO_H
#include <errno.h>
#endif
#if APR_HAVE_STRING_H
#include <string.h>
#endif
#if APR_HAVE_DIRENT_H
#include <dirent.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif


static apr_status_t dir_cleanup(void *thedir)
{
    apr_dir_t *dir = thedir;
    if (dir->dirhand != INVALID_HANDLE_VALUE && !FindClose(dir->dirhand)) {
        return apr_get_os_error();
    }
    dir->dirhand = INVALID_HANDLE_VALUE;
    return APR_SUCCESS;
} 

APR_DECLARE(apr_status_t) apr_dir_open(apr_dir_t **new, const char *dirname,
                                       apr_pool_t *pool)
{
    apr_status_t rv;

    apr_size_t len = strlen(dirname);
    (*new) = apr_pcalloc(pool, sizeof(apr_dir_t));
    /* Leave room here to add and pop the '*' wildcard for FindFirstFile 
     * and double-null terminate so we have one character to change.
     */
    (*new)->dirname = apr_palloc(pool, len + 3);
    memcpy((*new)->dirname, dirname, len);
    if (len && (*new)->dirname[len - 1] != '/') {
        (*new)->dirname[len++] = '/';
    }
    (*new)->dirname[len++] = '\0';
    (*new)->dirname[len] = '\0';

    /* Create a buffer for the longest file name we will ever see 
        */
    (*new)->entry = apr_pcalloc(pool, sizeof(WIN32_FIND_DATAW));
    (*new)->name = apr_pcalloc(pool, APR_FILE_MAX * 3 + 1);
    (*new)->rootlen = len - 1;
    (*new)->pool = pool;
    (*new)->dirhand = INVALID_HANDLE_VALUE;
    apr_pool_cleanup_register((*new)->pool, (void *)(*new), dir_cleanup,
                        apr_pool_cleanup_null);

    rv = apr_dir_read(NULL, 0, *new);
    if (rv != APR_SUCCESS) {
        dir_cleanup(*new);
        *new = NULL;
    }

    return rv;
}

APR_DECLARE(apr_status_t) apr_dir_close(apr_dir_t *dir)
{
    apr_pool_cleanup_kill(dir->pool, dir, dir_cleanup);
    return dir_cleanup(dir);
}

APR_DECLARE(apr_status_t) apr_dir_read(apr_finfo_t *finfo, apr_int32_t wanted,
                                       apr_dir_t *thedir)
{
    apr_status_t rv;
    char *fname;
    apr_wchar_t wdirname[APR_PATH_MAX];
    apr_wchar_t* eos = NULL;

    /* The while loops below allow us to skip all invalid file names, so that
     * we aren't reporting any files where their absolute paths are too long.
     */

    /* This code path is always be invoked by apr_dir_open or
     * apr_dir_rewind, so return without filling out the finfo.
     */
    if (thedir->dirhand == INVALID_HANDLE_VALUE) 
    {
        if ((rv = utf8_to_unicode_path(wdirname, sizeof(wdirname) 
                                                / sizeof(apr_wchar_t), 
                                        thedir->dirname))) {
            return rv;
        }
        eos = wcschr(wdirname, '\0');
        eos[0] = '*';
        eos[1] = '\0';

        thedir->dirhand = FindFirstFileExW(wdirname, FindExInfoBasic,
                                           thedir->entry,
                                           FindExSearchNameMatch, NULL,
                                           0);
        eos[0] = '\0';
        if (thedir->dirhand == INVALID_HANDLE_VALUE) {
            return apr_get_os_error();
        }
        thedir->bof = 1;
        return APR_SUCCESS;
    }
    else if (thedir->bof) {
        /* Noop - we already called FindFirstFileW from
         * either apr_dir_open or apr_dir_rewind ... use
         * that first record.
         */
        thedir->bof = 0; 
    }
    else if (!FindNextFileW(thedir->dirhand, thedir->entry)) {
        return apr_get_os_error();
    }

    while (thedir->rootlen &&
           thedir->rootlen + wcslen(thedir->entry->cFileName) >= APR_PATH_MAX)
    {
        if (!FindNextFileW(thedir->dirhand, thedir->entry)) {
            return apr_get_os_error();
        }
    }
    if ((rv = unicode_to_utf8_path(thedir->name, APR_FILE_MAX * 3 + 1, 
                                   thedir->entry->cFileName)))
        return rv;
    fname = thedir->name;

    fillin_fileinfo(finfo, (WIN32_FILE_ATTRIBUTE_DATA *) thedir->entry,
                    0, 1, fname, wanted);
    finfo->pool = thedir->pool;

    finfo->valid |= APR_FINFO_NAME;
    finfo->name = fname;

    if (wanted &= ~finfo->valid) {
        /* Go back and get more_info if we can't answer the whole inquiry
         */
        /* Almost all our work is done.  Tack on the wide file name
         * to the end of the wdirname (already / delimited)
         */
        if (!eos) {
            /* It's more efficient to store WDIRNAME in THEDIR,
             * but let's make simple fix first. */
            if ((rv = utf8_to_unicode_path(wdirname, sizeof(wdirname)
                                                     / sizeof(apr_wchar_t),
                                           thedir->dirname))) {
                return rv;
            }

            eos = wcschr(wdirname, '\0');
        }

        wcscpy(eos, thedir->entry->cFileName);
        rv = more_finfo(finfo, wdirname, wanted, MORE_OF_WFSPEC);
        eos[0] = '\0';
        return rv;
    }

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_dir_rewind(apr_dir_t *dir)
{
    apr_status_t rv;

    /* this will mark the handle as invalid and we'll open it
     * again if apr_dir_read() is subsequently called
     */
    rv = dir_cleanup(dir);

    if (rv == APR_SUCCESS)
        rv = apr_dir_read(NULL, 0, dir);

    return rv;
}

APR_DECLARE(apr_status_t) apr_dir_make(const char *path, apr_fileperms_t perm,
                                       apr_pool_t *pool)
{
    apr_wchar_t wpath[APR_PATH_MAX];
    apr_status_t rv;
    if ((rv = utf8_to_unicode_path(wpath,
                                    sizeof(wpath) / sizeof(apr_wchar_t),
                                    path))) {
        return rv;
    }
    if (!CreateDirectoryW(wpath, NULL)) {
        return apr_get_os_error();
    }

    return APR_SUCCESS;
}


static apr_status_t dir_make_parent(char *path,
                                    apr_fileperms_t perm,
                                    apr_pool_t *pool)
{
    apr_status_t rv;
    char *ch = strrchr(path, '\\');
    if (!ch) {
        return APR_ENOENT;
    }

    *ch = '\0';
    rv = apr_dir_make (path, perm, pool); /* Try to make straight off */
    
    if (APR_STATUS_IS_ENOENT(rv)) { /* Missing an intermediate dir */
        rv = dir_make_parent(path, perm, pool);

        if (rv == APR_SUCCESS || APR_STATUS_IS_EEXIST(rv)) {
            rv = apr_dir_make(path, perm, pool); /* And complete the path */
        }
    }

    *ch = '\\'; /* Always replace the slash before returning */
    return rv;
}

APR_DECLARE(apr_status_t) apr_dir_make_recursive(const char *path,
                                                 apr_fileperms_t perm,
                                                 apr_pool_t *pool)
{
    apr_status_t rv = 0;

    rv = apr_dir_make (path, perm, pool); /* Try to make PATH right out */

    if (APR_STATUS_IS_ENOENT(rv)) { /* Missing an intermediate dir */
        char *dir;

        rv = apr_filepath_merge(&dir, "", path, APR_FILEPATH_NATIVE, pool);

        if (rv != APR_SUCCESS)
            return rv;

        rv = dir_make_parent(dir, perm, pool); /* Make intermediate dirs */

        if (rv == APR_SUCCESS || APR_STATUS_IS_EEXIST(rv)) {
            rv = apr_dir_make (dir, perm, pool);   /* And complete the path */

            if (APR_STATUS_IS_EEXIST(rv)) {
                rv = APR_SUCCESS; /* Timing issue; see comment below */
            }
        }
    }
    else if (APR_STATUS_IS_EEXIST(rv)) {
        /*
         * It's OK if PATH exists. Timing issues can lead to the
         * second apr_dir_make being called on existing dir, therefore
         * this check has to come last.
         */
        rv = APR_SUCCESS;
    }

    return rv;
}


APR_DECLARE(apr_status_t) apr_dir_remove(const char *path, apr_pool_t *pool)
{
    apr_wchar_t wpath[APR_PATH_MAX];
    apr_status_t rv;

    if ((rv = utf8_to_unicode_path(wpath,
                                    sizeof(wpath) / sizeof(apr_wchar_t),
                                    path))) {
        return rv;
    }
    if (!RemoveDirectoryW(wpath)) {
        return apr_get_os_error();
    }

    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_os_dir_get(apr_os_dir_t **thedir,
                                         apr_dir_t *dir)
{
    if (dir == NULL) {
        return APR_ENODIR;
    }
    *thedir = dir->dirhand;
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_os_dir_put(apr_dir_t **dir,
                                         apr_os_dir_t *thedir,
                                         apr_pool_t *pool)
{
    return APR_ENOTIMPL;
}
