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

#include "apr_arch_networkio.h"
#include "apr_errno.h"
#include "apr_general.h"
#include "apr_network_io.h"
#include "apr_lib.h"
#include "apr_arch_file_io.h"
#if APR_HAVE_TIME_H
#include <time.h>
#endif

/* MAX_SEGMENT_SIZE is the maximum amount of data that will be sent to a client
 * in one call of TransmitFile. This number must be small enough to give the 
 * slowest client time to receive the data before the socket timeout triggers.
 * The same problem can exist with apr_socket_send(). In that case, we rely on
 * the application to adjust socket timeouts and max send segment 
 * sizes appropriately.
 * For example, Apache will in most cases call apr_socket_send() with less
 * than 8193 bytes.
 */
#define MAX_SEGMENT_SIZE 65536
#define WSABUF_ON_STACK 50

APR_DECLARE(apr_status_t) apr_socket_send(apr_socket_t *sock, const char *buf,
                                          apr_size_t *len)
{
    apr_ssize_t rv;
    WSABUF wsaData;
    int lasterror;
    DWORD dwBytes = 0;

    wsaData.len = (u_long)*len;
    wsaData.buf = (char*) buf;

    rv = WSASend(sock->socketdes, &wsaData, 1, &dwBytes, 0, NULL, NULL);
    if (rv == SOCKET_ERROR) {
        lasterror = apr_get_netos_error();
        *len = 0;
        return lasterror;
    }

    *len = dwBytes;

    return APR_SUCCESS;
}


APR_DECLARE(apr_status_t) apr_socket_recv(apr_socket_t *sock, char *buf,
                                          apr_size_t *len) 
{
    apr_ssize_t rv;
    WSABUF wsaData;
    int lasterror;
    DWORD dwBytes = 0;
    DWORD flags = 0;

    wsaData.len = (u_long)*len;
    wsaData.buf = (char*) buf;

    rv = WSARecv(sock->socketdes, &wsaData, 1, &dwBytes, &flags, NULL, NULL);
    if (rv == SOCKET_ERROR) {
        lasterror = apr_get_netos_error();
        *len = 0;
        return lasterror;
    }

    *len = dwBytes;
    return dwBytes == 0 ? APR_EOF : APR_SUCCESS;
}


APR_DECLARE(apr_status_t) apr_socket_sendv(apr_socket_t *sock,
                                           const struct iovec *vec,
                                           apr_int32_t in_vec, apr_size_t *nbytes)
{
    apr_status_t rc = APR_SUCCESS;
    apr_ssize_t rv;
    apr_size_t total_len;
    apr_int32_t i;
    DWORD dwBytes = 0;
    WSABUF *pWsaBuf;

    total_len = 0;
    for (i = 0; i < in_vec; i++) {
        apr_size_t iov_len = vec[i].iov_len;
        if (iov_len > (apr_size_t)MAXDWORD - total_len) {
            /* WSASend() returns NumberOfBytesSent as DWORD, so the total size
               should be less than that. */
            return APR_EINVAL;
        }
        total_len += iov_len;
    }

    pWsaBuf = (in_vec <= WSABUF_ON_STACK) ? _alloca(sizeof(WSABUF) * (in_vec))
                                          : malloc(sizeof(WSABUF) * (in_vec));
    if (!pWsaBuf)
        return APR_ENOMEM;

    for (i = 0; i < in_vec; i++) {
        pWsaBuf[i].buf = vec[i].iov_base;
        pWsaBuf[i].len = (ULONG) vec[i].iov_len;
    }
    rv = WSASend(sock->socketdes, pWsaBuf, in_vec, &dwBytes, 0, NULL, NULL);
    if (rv == SOCKET_ERROR) {
        rc = apr_get_netos_error();
    }
    if (in_vec > WSABUF_ON_STACK)
        free(pWsaBuf);

    *nbytes = dwBytes;
    return rc;
}


APR_DECLARE(apr_status_t) apr_socket_sendto(apr_socket_t *sock,
                                            apr_sockaddr_t *where,
                                            apr_int32_t flags, const char *buf, 
                                            apr_size_t *len)
{
    apr_ssize_t rv;

    rv = sendto(sock->socketdes, buf, (int)*len, flags, 
                (const struct sockaddr*)&where->sa, 
                where->salen);
    if (rv == SOCKET_ERROR) {
        *len = 0;
        return apr_get_netos_error();
    }

    *len = rv;
    return APR_SUCCESS;
}


APR_DECLARE(apr_status_t) apr_socket_recvfrom(apr_sockaddr_t *from, 
                                              apr_socket_t *sock,
                                              apr_int32_t flags, 
                                              char *buf, apr_size_t *len)
{
    apr_ssize_t rv;

    from->salen = sizeof(from->sa);

    rv = recvfrom(sock->socketdes, buf, (int)*len, flags, 
                  (struct sockaddr*)&from->sa, &from->salen);
    if (rv == SOCKET_ERROR) {
        (*len) = 0;
        return apr_get_netos_error();
    }

    apr_sockaddr_vars_set(from, from->sa.sin.sin_family, 
                          ntohs(from->sa.sin.sin_port));

    (*len) = rv;
    if (rv == 0 && sock->type == SOCK_STREAM)
        return APR_EOF;

    return APR_SUCCESS;
}


#if APR_HAS_SENDFILE
static apr_status_t collapse_iovec(char **off, apr_size_t *len, 
                                   struct iovec *iovec, int numvec, 
                                   char *buf, apr_size_t buflen)
{
    if (numvec == 1) {
        *off = iovec[0].iov_base;
        *len = iovec[0].iov_len;
    }
    else {
        int i;
        for (i = 0; i < numvec; i++) {
            *len += iovec[i].iov_len;
        }

        if (*len > buflen) {
            *len = 0;
            return APR_INCOMPLETE;
        }

        *off = buf;

        for (i = 0; i < numvec; i++) {
            memcpy(buf, iovec[i].iov_base, iovec[i].iov_len);
            buf += iovec[i].iov_len;
        }
    }
    return APR_SUCCESS;
}


/*
 * apr_status_t apr_socket_sendfile(apr_socket_t *, apr_file_t *, apr_hdtr_t *, 
 *                                 apr_off_t *, apr_size_t *, apr_int32_t flags)
 *    Send a file from an open file descriptor to a socket, along with 
 *    optional headers and trailers
 * arg 1) The socket to which we're writing
 * arg 2) The open file from which to read
 * arg 3) A structure containing the headers and trailers to send
 * arg 4) Offset into the file where we should begin writing
 * arg 5) Number of bytes to send out of the file
 * arg 6) APR flags that are mapped to OS specific flags
 */
APR_DECLARE(apr_status_t) apr_socket_sendfile(apr_socket_t *sock, 
                                              apr_file_t *file,
                                              apr_hdtr_t *hdtr,
                                              apr_off_t *offset,
                                              apr_size_t *len,
                                              apr_int32_t flags) 
{
    apr_status_t status = APR_SUCCESS;
    apr_status_t rv;
    apr_off_t curoff = *offset;
    DWORD dwFlags = 0;
    apr_size_t nbytes;
    TRANSMIT_FILE_BUFFERS tfb, *ptfb = NULL;
    apr_size_t bytes_to_send;   /* Bytes to send out of the file (not including headers) */
    int sendv_trailers = 0;
    char hdtrbuf[4096];
    LPFN_TRANSMITFILE pfn_transmit_file = NULL;
    static GUID wsaid_transmitfile = WSAID_TRANSMITFILE;
    DWORD dw;

    /* According to documentation TransmitFile() should not be used directly.
     * Pointer to function should retrieved using WSAIoctl:
     * https://docs.microsoft.com/en-gb/windows/win32/api/mswsock/nf-mswsock-transmitfile#remarks
     */
    if (WSAIoctl(sock->socketdes, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &wsaid_transmitfile, sizeof(wsaid_transmitfile),
                 &pfn_transmit_file, sizeof(pfn_transmit_file),
                 &dw, NULL, NULL) == SOCKET_ERROR) {
        return apr_get_os_error();
    }

    if (dw != sizeof(pfn_transmit_file)) {
        return APR_EINVAL;
    }

    /* Use len to keep track of number of total bytes sent (including headers) */
    bytes_to_send = *len;
    *len = 0;

    /* According to the documentation [1] TF_USE_KERNEL_APC is the most performant option:
     * [[[
     * Directs the driver to use kernel asynchronous procedure calls (APCs)
     * instead of worker threads to process long TransmitFile requests.
     * Long TransmitFile requests are defined as requests that require more
     * than a single read from the file or a cache; the request therefore
     * depends on the size of the file and the specified length of the send
     * packet. 
     *
     * Use of TF_USE_KERNEL_APC can deliver significant performance benefits.
     * It is possible (though unlikely), however, that the thread in which
     * context TransmitFile is initiated is being used for heavy computations;
     * this situation may prevent APCs from launching.
     * ]]]
     *
     * The downside is not applicable for our use case since calling thread
     * is not busy and waiting for operation completion.
     *
     * [1] https://docs.microsoft.com/en-gb/windows/win32/api/mswsock/nf-mswsock-transmitfile
     */
    dwFlags |= TF_USE_KERNEL_APC;

    /* Handle the goofy case of sending headers/trailers and a zero byte file */
    if (!bytes_to_send && hdtr) {
        if (hdtr->numheaders) {
            rv = apr_socket_sendv(sock, hdtr->headers, hdtr->numheaders, 
                                  &nbytes);
            if (rv != APR_SUCCESS)
                return rv;
            *len += nbytes;
        }
        if (hdtr->numtrailers) {
            rv = apr_socket_sendv(sock, hdtr->trailers, hdtr->numtrailers,
                                  &nbytes);
            if (rv != APR_SUCCESS)
                return rv;
            *len += nbytes;
        }
        return APR_SUCCESS;
    }

    memset(&tfb, '\0', sizeof (tfb));

    /* Collapse the headers into a single buffer */
    if (hdtr && hdtr->numheaders) {
        apr_size_t head_length = tfb.HeadLength;
        ptfb = &tfb;
        nbytes = 0;
        rv = collapse_iovec((char **)&ptfb->Head, &head_length, 
                            hdtr->headers, hdtr->numheaders, 
                            hdtrbuf, sizeof(hdtrbuf));

        tfb.HeadLength = (DWORD)head_length;

        /* If not enough buffer, punt to sendv */
        if (rv == APR_INCOMPLETE) {
            rv = apr_socket_sendv(sock, hdtr->headers, hdtr->numheaders, &nbytes);
            if (rv != APR_SUCCESS)
                return rv;
            *len += nbytes;
            ptfb = NULL;
        }
    }

    /* Initialize the overlapped structure used on TransmitFile
     */
    if (!sock->overlapped) {
        sock->overlapped = apr_pcalloc(sock->pool, sizeof(OVERLAPPED));
        sock->overlapped->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    }
    while (bytes_to_send) {
        DWORD xmitbytes;

        if (bytes_to_send > MAX_SEGMENT_SIZE) {
            xmitbytes = MAX_SEGMENT_SIZE;
        }
        else {
            /* Last call to TransmitFile() */
            xmitbytes = (DWORD)bytes_to_send;
            /* Collapse the trailers into a single buffer */
            if (hdtr && hdtr->numtrailers) {
                apr_size_t tail_length = tfb.TailLength;
                ptfb = &tfb;
                rv = collapse_iovec((char**) &ptfb->Tail, &tail_length,
                                    hdtr->trailers, hdtr->numtrailers,
                                    hdtrbuf + ptfb->HeadLength,
                                    sizeof(hdtrbuf) - ptfb->HeadLength);

                tfb.TailLength = (DWORD)tail_length;

                if (rv == APR_INCOMPLETE) {
                    /* If not enough buffer, punt to sendv, later */
                    sendv_trailers = 1;
                }
            }
        }

        sock->overlapped->Offset = (DWORD)(curoff);
#if APR_HAS_LARGE_FILES
        sock->overlapped->OffsetHigh = (DWORD)(curoff >> 32);
#endif  
        /* XXX BoundsChecker claims dwFlags must not be zero. */
        rv = pfn_transmit_file(sock->socketdes,  /* socket */
                               file->filehand, /* open file descriptor of the file to be sent */
                               xmitbytes,      /* number of bytes to send. 0=send all */
                               0,              /* Number of bytes per send. 0=use default */
                               sock->overlapped,    /* OVERLAPPED structure */
                               ptfb,           /* header and trailer buffers */
                               dwFlags);       /* flags to control various aspects of TransmitFile */
        if (!rv) {
            status = apr_get_netos_error();
            if ((status == APR_FROM_OS_ERROR(ERROR_IO_PENDING)) ||
                (status == APR_FROM_OS_ERROR(WSA_IO_PENDING))) 
            {
                rv = WaitForSingleObject(sock->overlapped->hEvent, 
                                         (DWORD)(sock->timeout >= 0 
                                                 ? sock->timeout_ms : INFINITE));
                if (rv == WAIT_OBJECT_0) {
                    status = APR_SUCCESS;
                    if (!WSAGetOverlappedResult(sock->socketdes,
                                                sock->overlapped,
                                                &xmitbytes,
                                                FALSE,
                                                &dwFlags)) {
                        status = apr_get_netos_error();
                    }
                    /* Ugly code alert: WSAGetOverlappedResult returns
                     * a count of all bytes sent. This loop only
                     * tracks bytes sent out of the file.
                     */
                    else if (ptfb) {
                        xmitbytes -= (ptfb->HeadLength + ptfb->TailLength);
                    }
                }
                else if (rv == WAIT_TIMEOUT) {
                    status = APR_FROM_OS_ERROR(WAIT_TIMEOUT);
                }
                else if (rv == WAIT_ABANDONED) {
                    /* Hummm... WAIT_ABANDONDED is not an error code. It is
                     * a return specific to the Win32 WAIT functions that
                     * indicates that a thread exited while holding a
                     * mutex. Should consider triggering an assert
                     * to detect the condition...
                     */
                    status = APR_FROM_OS_ERROR(WAIT_TIMEOUT);
                }
                else
                    status = apr_get_os_error();
            }
        }
        if (status != APR_SUCCESS)
            break;

        bytes_to_send -= xmitbytes;
        curoff += xmitbytes;
        *len += xmitbytes;
        /* Adjust len for any headers/trailers sent */
        if (ptfb) {
            *len += (ptfb->HeadLength + ptfb->TailLength);
            memset(&tfb, '\0', sizeof (tfb));
            ptfb = NULL;
        }
    }

    if (status == APR_SUCCESS) {
        if (sendv_trailers) {
            rv = apr_socket_sendv(sock, hdtr->trailers, hdtr->numtrailers, &nbytes);
            if (rv != APR_SUCCESS)
                return rv;
            *len += nbytes;
        }
    }

    return status;
}

#endif

APR_DECLARE(apr_status_t) apr_socket_wait(apr_socket_t *sock, apr_wait_type_t direction)
{
    fd_set fdset, *rptr, *wptr;
    int rc;
    struct timeval tv, *tvptr;

    FD_ZERO(&fdset);
    FD_SET(sock->socketdes, &fdset);

    if (direction == APR_WAIT_READ) {
        rptr = &fdset;
        wptr = NULL;
    }
    else { /* APR_WAIT_WRITE */
        rptr = NULL;
        wptr = &fdset;
    }

    if (sock->timeout < 0) {
        tvptr = NULL;
    }
    else {
        /* casts for winsock/timeval definition */
        tv.tv_sec =  (long)apr_time_sec(sock->timeout);
        tv.tv_usec = (int)apr_time_usec(sock->timeout);
        tvptr = &tv;
    }
    rc = select(/* ignored */ FD_SETSIZE+1, rptr, wptr, NULL, tvptr);
    if (rc == SOCKET_ERROR) {
        return apr_get_netos_error();
    }
    else if (!rc) {
        return APR_FROM_OS_ERROR(WSAETIMEDOUT);
    }

    return APR_SUCCESS;
}

