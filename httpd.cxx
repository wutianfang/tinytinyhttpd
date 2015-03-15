/* Copyright 2009 by Yasuhiro Matsumoto
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "httpd.h"
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <signal.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif

#if defined (__SVR4) && defined (__sun)
#define __solaris__
#endif

#if defined LINUX_SENDFILE_API
#include <sys/sendfile.h>
#elif defined FREEBSD_SENDFILE_API
#include <sys/uio.h>
#elif defined _WIN32
#include <mswsock.h>
#ifndef WSAID_TRANSMITFILE
#define WSAID_TRANSMITFILE \
{0xb5367df0,0xcbac,0x11cf,{0x95,0xca,0x00,0x80,0x5f,0x48,0xa1,0x92}}

typedef
BOOL
(PASCAL FAR * LPFN_TRANSMITFILE)(
        IN SOCKET hSocket,
        IN HANDLE hFile,
        IN DWORD nNumberOfBytesToWrite,
        IN DWORD nNumberOfBytesPerSend,
        IN LPOVERLAPPED lpOverlapped,
        IN LPTRANSMIT_FILE_BUFFERS lpTransmitBuffers,
        IN DWORD dwReserved
        );
#endif
static LPFN_TRANSMITFILE lpfnTransmitFile = NULL;
#endif

extern char* crypt(const char *key, const char *setting);

namespace tthttpd {

#ifdef _WIN32
    typedef int socklen_t;
#else
#define closesocket(x) close(x)
#define strnicmp(x, y, z) strncasecmp(x, y, z)
#endif

#if !defined(EWOULDBLOCK) && defined(WSAEWOULDBLOCK)
#define EWOULDBLOCK WSAEWOULDBLOCK
#endif

#ifndef NBBY
#define NBBY    8          /* number of bits in a byte */
#endif
#ifndef NFDBITS
#define NFDBITS (sizeof(fd_mask) * NBBY)        /* bits per mask */
#endif

#ifndef howmany
#define howmany(x,y)    (((x)+((y)-1))/(y))
#endif

    typedef long fd_mask;

#ifndef S_ISREG
#define S_ISREG(x) (x & S_IFREG)
#endif

#define VERBOSE(x) (httpd->verbose_mode >= x)

#if !defined(HAVE_GETADDRINFO) && defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0501
    int inet_aton(const char *cp, struct in_addr *addr) {
        register unsigned int val;
        register int base, n;
        register char c;
        unsigned int parts[4];
        register unsigned int *pp = parts;

        c = *cp;
        for (;;) {
            if (!isdigit(c))
                return (0);
            val = 0; base = 10;
            if (c == '0') {
                c = *++cp;
                if (c == 'x' || c == 'X')
                    base = 16, c = *++cp;
                else
                    base = 8;
            }
            for (;;) {
                if (isascii(c) && isdigit(c)) {
                    val = (val * base) + (c - '0');
                    c = *++cp;
                } else if (base == 16 && isascii(c) && isxdigit(c)) {
                    val = (val << 4) |
                        (c + 10 - (islower(c) ? 'a' : 'A'));
                    c = *++cp;
                } else
                    break;
            }
            if (c == '.') {
                if (pp >= parts + 3)
                    return (0);
                *pp++ = val;
                c = *++cp;
            } else
                break;
        }
        if (c != '\0' && (!isascii(c) || !isspace(c)))
            return (0);
        n = pp - parts + 1;
        switch (n) {

            case 0:
                return (0);    /* initial nondigit */

            case 1:        /* a -- 32 bits */
                break;

            case 2:        /* a.b -- 8.24 bits */
                if ((val > 0xffffff) || (parts[0] > 0xff))
                    return (0);
                val |= parts[0] << 24;
                break;

            case 3:        /* a.b.c -- 8.8.16 bits */
                if ((val > 0xffff) || (parts[0] > 0xff) || (parts[1] > 0xff))
                    return (0);
                val |= (parts[0] << 24) | (parts[1] << 16);
                break;

            case 4:        /* a.b.c.d -- 8.8.8.8 bits */
                if ((val > 0xff) || (parts[0] > 0xff) || (parts[1] > 0xff) || (parts[2] > 0xff))
                    return (0);
                val |= (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8);
                break;
        }
        if (addr)
            addr->s_addr = htonl(val);
        return (1);
    }

    const char* gai_strerror(int ecode) {
        switch (ecode) {
            case EAI_NODATA:
                return "no address associated with hostname.";
            case EAI_MEMORY:
                return "memory allocation failure.";
            default:
                return "unknown error.";
        }
    }

    void freeaddrinfo(struct addrinfo *ai) {
        struct addrinfo *next;

        do {
            next = ai->ai_next;
            free(ai);
        } while (NULL != (ai = next));
    }

    static struct addrinfo *malloc_ai(int port, u_long addr) {
        struct addrinfo *ai;

        ai = (struct addrinfo*) malloc(sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
        if (ai == NULL)
            return(NULL);

        memset(ai, 0, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));

        ai->ai_addr = (struct sockaddr *)(ai + 1);
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr->sa_family = ai->ai_family = AF_INET;

        ((struct sockaddr_in *)(ai)->ai_addr)->sin_port = port;
        ((struct sockaddr_in *)(ai)->ai_addr)->sin_addr.s_addr = addr;

        return(ai);
    }

    int getnameinfo(const struct sockaddr *sa, size_t salen, char *host, 
            size_t hostlen, char *serv, size_t servlen, int flags) {
        struct sockaddr_in *sin = (struct sockaddr_in *)sa;
        struct hostent *hp;
        char tmpserv[16];

        if (serv) {
            snprintf(tmpserv, sizeof(tmpserv), "%d", ntohs(sin->sin_port));
            if (strlen(tmpserv) > servlen)
                return EAI_MEMORY;
            else
                strcpy(serv, tmpserv);
        }

        if (host) {
            if (flags & NI_NUMERICHOST) {
                if (strlen(inet_ntoa(sin->sin_addr)) > hostlen)
                    return EAI_MEMORY;

                strcpy(host, inet_ntoa(sin->sin_addr));
                return 0;
            } else {
                hp = gethostbyaddr((char *)&sin->sin_addr,
                        sizeof(struct in_addr), AF_INET);
                if (hp == NULL)
                    return EAI_NODATA;

                if (strlen(hp->h_name) > hostlen)
                    return EAI_MEMORY;

                strcpy(host, hp->h_name);
                return 0;
            }
        }
        return 0;
    }

    int getaddrinfo(const char *hostname, const char *servname,
            const struct addrinfo *hints, struct addrinfo **res) {
        struct addrinfo *cur, *prev = NULL;
        struct hostent *hp;
        struct in_addr in;
        int i, port;

        if (servname) {
            struct servent *se;
            if ((se = getservbyname(servname, "tcp")))
                port = se->s_port;
            else
                port = htons(atoi(servname));
        } else
            port = 0;

        if (hints && hints->ai_flags & AI_PASSIVE) {
            if (NULL != (*res = malloc_ai(port, htonl(0x00000000))))
                return 0;
            else
                return EAI_MEMORY;
        }

        if (!hostname) {
            if (NULL != (*res = malloc_ai(port, htonl(0x7f000001))))
                return 0;
            else
                return EAI_MEMORY;
        }

        if (inet_aton(hostname, &in)) {
            if (NULL != (*res = malloc_ai(port, in.s_addr)))
                return 0;
            else
                return EAI_MEMORY;
        }

        hp = gethostbyname(hostname);
        if (hp && hp->h_name && hp->h_name[0] && hp->h_addr_list[0]) {
            for (i = 0; hp->h_addr_list[i]; i++) {
                cur = malloc_ai(port, ((struct in_addr *)hp->h_addr_list[i])->s_addr);
                if (cur == NULL) {
                    if (*res)
                        freeaddrinfo(*res);
                    return EAI_MEMORY;
                }
                if (prev) prev->ai_next = cur;
                else *res = cur;
                prev = cur;
            }
            return 0;
        }

        return EAI_NODATA;
    }
#endif

    typedef struct {
#ifdef _WIN32
        HANDLE read;
        HANDLE write;
        HANDLE process;
#else
        int read;
        int write;
        pid_t process;
#endif
        unsigned long size;
    } RES_INFO;

    bool operator<(const server::ListInfo& left, const server::ListInfo& right) {
        return left.name < right.name;
    }

    bool operator>(const server::ListInfo& left, const server::ListInfo& right) {
        return left.name > right.name;
    }

    const char * const months[]={
        "Jan",
        "Feb",
        "Mar",
        "Apr",
        "May",
        "Jun",
        "Jul",
        "Aug",
        "Sep",
        "Oct",
        "Nov",
        "Dec"};
    const char * const wdays[]={
        "Sun",
        "Mon",
        "Tue",
        "Wed",
        "Thu",
        "Fri",
        "Sat"};

#ifdef _WIN32
    static bool filetime2unixtime(const FILETIME* ft, struct tm* tm) {
        FILETIME lt;
        SYSTEMTIME st;

        if (!FileTimeToLocalFileTime(ft, &lt)) return false;
        if (!FileTimeToSystemTime(&lt, &st)) return false;
        memset(tm, 0, sizeof(tm));
        tm->tm_year = st.wYear - 1900;
        tm->tm_mon = st.wMonth - 1;
        tm->tm_mday = st.wDay;
        tm->tm_hour = st.wHour;
        tm->tm_min = st.wMinute;
        tm->tm_sec = st.wSecond;
        return true;
    }
#endif

    static std::string res_curtime(int diff = 0) {
        time_t tt = time(NULL) + diff;
        struct tm* p = gmtime(&tt);

        char buf[256];
        sprintf(buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                wdays[p->tm_wday],
                p->tm_mday,
                months[p->tm_mon],
                p->tm_year+1900,
                p->tm_hour,
                p->tm_min,
                p->tm_sec);
        return buf;
    }

    static void my_perror(std::string mes) {
#ifdef _WIN32
        void*  pMsgBuf;
        FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                NULL,
                GetLastError(),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
                (LPSTR) &pMsgBuf,
                0,
                NULL
                );

        std::string err = (LPSTR)pMsgBuf;
        LocalFree(pMsgBuf);
        err.erase(std::remove(err.begin(), err.end(), '\r'), err.end());
        err.erase(std::remove(err.begin(), err.end(), '\n'), err.end());
        fprintf(stderr, "%s: %s\n", mes.c_str(), err.c_str());
#else
        perror(mes.c_str());
#endif
    }

#ifdef _WIN32
    static RES_INFO* res_fopen(std::string& file) {
        HANDLE hFile;
        hFile = CreateFileA(
                file.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED,
                NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return NULL;

        RES_INFO* res_info = new RES_INFO;
        res_info->read = hFile;
        res_info->write = 0;
        res_info->process = 0;
        res_info->size = (unsigned long)-1;
        return res_info;
    }

    static bool res_isfile(std::string& file) {
        DWORD dwAttr = GetFileAttributesA(file.c_str());
        return (dwAttr != (DWORD)-1 && !(dwAttr & FILE_ATTRIBUTE_DIRECTORY));
    }

    static bool res_isdir(std::string& file) {
        DWORD dwAttr = GetFileAttributesA(file.c_str());
        return (dwAttr != (DWORD)-1 && (dwAttr & FILE_ATTRIBUTE_DIRECTORY));
    }

    static bool res_isexe(std::string& file, std::string& path_info, std::string& script_name) {
        std::vector<std::string> split_path;
        std::string path;
        const char* env = getenv("PATHEXT");
        std::string pathext = env ? env : "";

        std::vector<std::string> pathexts;
        std::vector<std::string>::iterator itext;

        split_string(file, "/", split_path);
        std::transform(pathext.begin(), pathext.end(), pathext.begin(), ::tolower);
        split_string(pathext, ";", pathexts);

        for (std::vector<std::string>::iterator it = split_path.begin(); it != split_path.end(); it++) {
            if (it->empty()) continue;
            if (!path.empty()) path += "/";
            path += *it;
            struct stat  st;
            if (stat((char *)path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                for (itext = pathexts.begin(); itext != pathexts.end(); itext++) {
                    if (path.substr(path.size() - itext->size()) == *itext) {
                        path_info = file.c_str() + path.size();
                        script_name.resize(script_name.size() - path_info.size());
                        file = path;
                        if (path_info.empty()) script_name += *it;
                        return true;
                    }
                }
            }
            for (itext = pathexts.begin(); itext != pathexts.end(); itext++) {
                std::string tmp = path + *itext;
                if (stat((char *)tmp.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    path_info = file.c_str() + path.size();
                    script_name.resize(script_name.size() - path_info.size());
                    file = tmp;
                    if (path_info.empty()) script_name += *it;
                    return true;
                }
            }
        }
        return false;
    }

    static bool res_iscgi(std::string& file, std::string& path_info, std::string& script_name, server::MimeTypes& mime_types, std::string& type) {
        std::vector<std::string> split_path;
        std::string path;

        split_string(file, "/", split_path);
        for (std::vector<std::string>::iterator it = split_path.begin(); it != split_path.end(); it++) {
            if (it->empty()) continue;
            if (!path.empty()) path += "/";
            path += *it;
            struct stat  st;
            if (stat((char *)path.c_str(), &st))
                continue;
            server::MimeTypes::iterator it_mime;
            for(it_mime = mime_types.begin(); it_mime != mime_types.end(); it_mime++) {
                if (it_mime->second[0] != '@') continue;
                std::string match = ".";
                match += it_mime->first;
                if (!strcmp(path.c_str()+path.size()-match.size(), match.c_str())) {
                    type = it_mime->second;
                    path_info = file.c_str() + path.size();
                    script_name.resize(script_name.size() - path_info.size());
                    file = path;
                    if (script_name == "/")
                        script_name += *it;
                    return true;
                }
            }
        }
        return false;
    }

    static std::vector<server::ListInfo> res_flist(std::string& path) {
        WIN32_FIND_DATAA fData;
        std::vector<server::ListInfo> ret;
        if (path.size() && path[path.size()-1] != '/')
            path += "/";
        std::string pattern = path + "*";
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fData);

        do {
            if (hFind == INVALID_HANDLE_VALUE) break;
            if (strcmp(fData.cFileName, ".")) {
                server::ListInfo listInfo;
                listInfo.name = fData.cFileName;
                listInfo.isdir = (fData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    ? true : false;
                listInfo.size = fData.nFileSizeLow;
                filetime2unixtime(&fData.ftLastWriteTime, &listInfo.date);
                ret.push_back(listInfo);
            }
        } while(FindNextFileA(hFind, &fData));
        if (hFind != INVALID_HANDLE_VALUE) FindClose(hFind);
        std::sort(ret.begin(), ret.end());
        return ret;
    }

    static unsigned long res_fsize(RES_INFO* res_info) {
        return GetFileSize(res_info->read, NULL);
    }

    static std::string res_ftime(std::string& file, int diff = 0) {
        HANDLE hFile;
        hFile = CreateFileA(
                file.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                NULL,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return "";
        FILETIME filetime = {0};
        SYSTEMTIME systemtime = {0};
        GetFileTime(hFile, NULL, NULL, &filetime);
        CloseHandle(hFile);
        FileTimeToSystemTime(&filetime, &systemtime);
        struct tm t = {0};
        t.tm_year = systemtime.wYear-1900;
        t.tm_mon = systemtime.wMonth;
        t.tm_mday = systemtime.wDay;
        t.tm_hour = systemtime.wHour;
        t.tm_min = systemtime.wMinute;
        t.tm_sec = systemtime.wSecond;
        t.tm_isdst = 0;
        time_t tt = mktime(&t) + diff;
        struct tm* p = localtime(&tt);
        //int offset= -(int)timezone;
        //offset = offset/60/60*100 + (offset/60)%60;

        char buf[256];
        //sprintf(buf, "%s, %02d %s %04d %02d:%02d:%02d %+05d",
        //  wdays[p->tm_wday],
        //  p->tm_mday,
        //  months[p->tm_mon-1],
        //  p->tm_year+1900,
        //  p->tm_hour,
        //  p->tm_min,
        //  p->tm_sec,
        //  offset);
        sprintf(buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                wdays[p->tm_wday],
                p->tm_mday,
                months[p->tm_mon],
                p->tm_year+1900,
                p->tm_hour,
                p->tm_min,
                p->tm_sec);
        return buf;
    }

    static std::string res_fgets(RES_INFO* res_info) {
        char c;
        std::stringstream ss;
        while (1) {
            DWORD dwRead;
            if (ReadFile(res_info->read, &c, 1, &dwRead, NULL) == FALSE) break;
            if (c == '\n') break;
            if (c != '\r') ss << c;
        }
        return ss.str();
    }

    static unsigned long res_write(RES_INFO* res_info, char* data, unsigned long size) {
        DWORD dwWrite = 0;
        WriteFile(res_info->write, data, size, &dwWrite, NULL);
        return dwWrite;
    }

    static long long res_read(RES_INFO* res_info, char* data, unsigned long size) {
        DWORD dwRead = 0;
        OVERLAPPED ovRead;
        memset(&ovRead, 0, sizeof(ovRead));
        if (res_info->process) {
            if (PeekNamedPipe(res_info->read, NULL, 0, NULL, &dwRead, NULL) == TRUE && dwRead == 0) {
                return 0;
            }
        }
        if (ReadFile(res_info->read, data, size, &dwRead, &ovRead) == TRUE) {
            return dwRead;
        }
        DWORD dwErr = GetLastError();
        if (dwErr != ERROR_IO_PENDING)
            return -1;
        return 0;
    }

    static RES_INFO* res_popen(std::vector<std::string>& args, std::vector<std::string>& envs) {
        int envs_len = 1;
        int n;
        char *envs_ptr;
        char *ptr;
        std::vector<std::string>::const_iterator it;
        std::string command;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        HANDLE hClientOut_rd, hClientOut_wr;
        HANDLE hClientIn_rd, hClientIn_wr;

        if(!CreatePipe(&hClientOut_rd, &hClientOut_wr, NULL, 0)) {
            return NULL;
        }

        if(!CreatePipe(&hClientIn_rd, &hClientIn_wr, NULL, 0)) {
            CloseHandle(hClientOut_rd);
            CloseHandle(hClientOut_wr);
            return NULL;
        }

#ifdef NT
        SetHandleInformation(
                hClientOut_wr,
                HANDLE_FLAG_INHERIT,
                HANDLE_FLAG_INHERIT);
        SetHandleInformation(
                hClientIn_rd,
                HANDLE_FLAG_INHERIT,
                HANDLE_FLAG_INHERIT);
#else
        DuplicateHandle(
                GetCurrentProcess(),
                hClientOut_wr,
                GetCurrentProcess(),
                &hClientOut_wr,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
        DuplicateHandle(
                GetCurrentProcess(),
                hClientIn_rd,
                GetCurrentProcess(),
                &hClientIn_rd,
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
#endif

        memset(&si,0,sizeof(STARTUPINFOA));
        si.cb         = sizeof(STARTUPINFOA);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = hClientIn_rd;
        si.hStdOutput = hClientOut_wr;
        si.hStdError  = hClientOut_wr;

        for(it = args.begin(), n = 0; it != args.end(); it++, n++) {
            if (it != args.begin()) command += " ";
            command += *it;
        }

        for(it = envs.begin(); it != envs.end(); it++)
            envs_len += (int)it->size() + 1;
        envs_ptr = new char[envs_len];
        memset(envs_ptr, 0, envs_len);
        ptr = envs_ptr;
        for(it = envs.begin(); it != envs.end(); it++) {
            strcpy(ptr, it->c_str());
            ptr += it->size() + 1;
        }

        std::string path = args.size() > 1  ? args[1] : args[0];
        size_t end_pos = path.find_last_of('/');
        if (end_pos != std::string::npos) path.erase(end_pos);

        BOOL bRet = CreateProcessA(
                NULL,
                (char*)command.c_str(),
                NULL,
                NULL,
                TRUE,
                NORMAL_PRIORITY_CLASS | DETACHED_PROCESS,
                envs_ptr,
                path.c_str(),
                &si,
                &pi);
        delete envs_ptr;
        if(!bRet) {
            my_perror("CreateProcessA");
            CloseHandle(hClientIn_rd);
            CloseHandle(hClientOut_wr);
            return NULL;
        }

        CloseHandle(pi.hThread);

        CloseHandle(hClientIn_rd);
        CloseHandle(hClientOut_wr);

        RES_INFO* res_info = new RES_INFO;
        res_info->read = hClientOut_rd;
        res_info->write = hClientIn_wr;
        res_info->process = pi.hProcess;
        res_info->size = (unsigned long)-1;
        return res_info;
    }

    static void res_closewriter(RES_INFO* res_info) {
        if (res_info && res_info->write) {
            CloseHandle(res_info->write);
            res_info->write = NULL;
        }
    }

    static void res_close(RES_INFO* res_info) {
        if (res_info) {
            if (res_info->read) CloseHandle(res_info->read);
            if (res_info->write) CloseHandle(res_info->write);
            if (res_info->process) CloseHandle(res_info->process);
            delete res_info;
        }
    }
#else
    static RES_INFO* res_fopen(std::string& file) {
        int fd = open(file.c_str(), O_RDONLY);
        if (fd < 0)
            return NULL;

        RES_INFO* res_info = new RES_INFO;
        res_info->read = fd;
        res_info->write = 0;
        res_info->process = 0;
        res_info->size = (unsigned long)-1;
        return res_info;
    }

    static bool res_isfile(std::string& file) {
        struct stat statbuf = {0};
        stat(file.c_str(), &statbuf);
        return statbuf.st_mode & S_IFREG;
    }

    static bool res_isdir(std::string& file) {
        struct stat statbuf = {0};
        stat(file.c_str(), &statbuf);
        return statbuf.st_mode & S_IFDIR;
    }

    static bool res_isexe(std::string& file, std::string& path_info, std::string& script_name) {
        std::vector<std::string> split_path;
        std::string path;

        split_string(file, "/", split_path);
        for (std::vector<std::string>::iterator it = split_path.begin(); it != split_path.end(); it++) {
            if (it->empty()) continue;
            path += "/";
            path += *it;
            struct stat  st;
            if (stat((char *)path.c_str(), &st))
                continue;
            if (S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0) {
                path_info = file.c_str() + path.size();
                script_name.resize(script_name.size() - path_info.size());
                file = path;
                if (path_info.empty()) script_name += *it;
                return true;
            }
        }
        return false;
    }

    static bool res_iscgi(std::string& file, std::string& path_info, std::string& script_name, server::MimeTypes& mime_types, std::string& type) {
        std::vector<std::string> split_path;
        std::string path;

        split_string(file, "/", split_path);
        for (std::vector<std::string>::iterator it = split_path.begin(); it != split_path.end(); it++) {
            if (it->empty()) continue;
            path += "/";
            path += *it;
            struct stat  st;
            if (stat((char *)path.c_str(), &st))
                continue;
            server::MimeTypes::iterator it_mime;
            for(it_mime = mime_types.begin(); it_mime != mime_types.end(); it_mime++) {
                if (it_mime->second[0] != '@') continue;
                std::string match = ".";
                match += it_mime->first;
                if (!strcmp(path.c_str()+path.size()-match.size(), match.c_str())) {
                    type = it_mime->second;
                    path_info = file.c_str() + path.size();
                    script_name.resize(script_name.size() - path_info.size());
                    file = path;
                    if (script_name == "/")
                        script_name += *it;
                    return true;
                }
            }
        }
        return false;
    }

    static std::vector<server::ListInfo> res_flist(std::string& path) {
        std::vector<server::ListInfo> ret;
        DIR* dir;
        struct dirent* dirp;
        if (!path.empty() && path[path.size()-1] != '/')
            path += "/";
        dir = opendir(path.c_str());
        while((dirp = readdir(dir))) {
            if (strcmp(dirp->d_name, ".")) {
                server::ListInfo listInfo;
                listInfo.name = dirp->d_name;
                std::string file = path + listInfo.name;
                struct stat statbuf = {0};
                stat(file.c_str(), &statbuf);
                listInfo.size = statbuf.st_size;
                memcpy(&listInfo.date, gmtime(&statbuf.st_mtime), sizeof(struct tm));
                listInfo.isdir = res_isdir(file);
                ret.push_back(listInfo);
            }
        }
        closedir(dir);
        sort(ret.begin(), ret.end());
        return ret;
    }

    static unsigned long res_fsize(RES_INFO* res_info) {
        struct stat statbuf = {0};
        fstat(res_info->read, &statbuf);
        return statbuf.st_size;
    }

    static std::string res_ftime(std::string& file, int diff = 0) {
        struct stat statbuf = {0};
        stat(file.c_str(), &statbuf);
        time_t tt = statbuf.st_mtime + diff;
        struct tm* p=gmtime(&tt);
        //int  offset;
        //int offset= -(int)timezone;
        //offset = offset/60/60*100 + (offset/60)%60;

        char buf[256];
        //sprintf(buf, "%s, %02d %s %04d %02d:%02d:%02d %+05d",
        //  wdays[p->tm_wday],
        //  p->tm_mday,
        //  months[p->tm_mon-1],
        //  p->tm_year+1900,
        //  p->tm_hour,
        //  p->tm_min,
        //  p->tm_sec,
        //  offset);
        sprintf(buf, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                wdays[p->tm_wday],
                p->tm_mday,
                months[p->tm_mon],
                p->tm_year+1900,
                p->tm_hour,
                p->tm_min,
                p->tm_sec);
        return buf;
    }

    static std::string res_fgets(RES_INFO* res_info) {
        std::stringstream ss;
        char c;
        while (1) {
            if (read(res_info->read, &c, 1) != 1) break;
            if (c == '\n') break;
            if (c != '\r') ss << c;
        }
        return ss.str();
    }

    static unsigned long res_write(RES_INFO* res_info, char* data, unsigned long size) {
        return write(res_info->write, data, size);
    }

    static long long res_read(RES_INFO* res_info, char* data, unsigned long size) {
        if (res_info->process) {
            int s = 0;
            if (waitpid(res_info->process, &s, WNOHANG) == -1) {
                return -1;
            }
        }
        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(res_info->read, &fdset);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        int r = select(FD_SETSIZE, &fdset, NULL, NULL, &tv);
        if (r == -1) return -1;
        if (FD_ISSET(res_info->read, &fdset)) {
            return (long long) read(res_info->read, data, size);
        }
        return 0;
    }

    static void res_popen_sigchild(int signo) {
        wait(0);
    }

    static RES_INFO* res_popen(std::vector<std::string>& args, std::vector<std::string>& envs) {
        int filedesr[2], filedesw[2];
        pid_t child;
        long flags;
        sigset_t newmask;
        char** args_ptr;
        char** envs_ptr;
        int n;
        std::vector<std::string>::iterator it;

        args_ptr = new char*[args.size() + 1];
        envs_ptr = new char*[envs.size() + 1];

        for(n = 0, it = args.begin(); it != args.end(); n++, it++)
            args_ptr[n] = (char*)it->c_str();
        args_ptr[args.size()] = 0;

        for(n = 0, it = envs.begin(); it != envs.end(); n++, it++)
            envs_ptr[n] = (char*)it->c_str();
        envs_ptr[envs.size()] = 0;

        int tmp;
        tmp = pipe(filedesr);
        tmp = pipe(filedesw);
        child = fork();
        if (!child) {
            dup2(filedesw[0], 0);
            dup2(filedesr[1], 1);
            dup2(filedesr[1], 2);
            close(filedesw[1]);
            close(filedesr[0]);
            setsid();
            setpgid(0, 0);
            sigemptyset(&newmask);
            sigaddset(&newmask, SIGTERM);
            sigaddset(&newmask, SIGKILL);
            pthread_sigmask(SIG_UNBLOCK, &newmask, 0L);
            usleep(500);
            std::string path = args.size() > 1 && args[1].at(0) == '/' ?
                args[1] : args[0];
            size_t end_pos = path.find_last_of('/');
            if (end_pos)
                path.erase(end_pos);
            tmp = chdir(path.c_str());
            if (execve(args_ptr[0], args_ptr, envs_ptr) < 0) {
                my_perror("execv");
            }
            delete[] envs_ptr;
            delete[] args_ptr;
        } else {
            signal(SIGCHLD, res_popen_sigchild);
            sigemptyset(&newmask);
            sigaddset(&newmask, SIGCHLD);
            sigaddset(&newmask, SIGTERM);
            sigaddset(&newmask, SIGKILL);
            pthread_sigmask(SIG_UNBLOCK, &newmask, 0L);
            close(filedesw[0]);
            close(filedesr[1]);

            flags = fcntl(filedesw[1], F_GETFL, 0);
            flags |= O_NONBLOCK;
#ifndef BSD
            flags |= O_NDELAY;
#endif
            fcntl(filedesw[1], F_SETFL, flags);

            RES_INFO* res_info = new RES_INFO;
            res_info->read = filedesr[0];
            res_info->write = filedesw[1];
            res_info->process = child;
            res_info->size = (unsigned long)-1;
            return res_info;
        }
        return NULL;
    }

    static void res_closewriter(RES_INFO* res_info) {
        if (res_info && res_info->write) {
            close(res_info->write);
            res_info->write = NULL;
        }
    }

    static void res_close(RES_INFO* res_info) {
        if (res_info) {
            if (res_info->read) close(res_info->read);
            if (res_info->write) close(res_info->write);
            delete res_info;
        }
    }

#endif

    static bool get_line(int fd, std::string& s) {
        char c = 0;
        std::stringstream ss;
        while (1) {
            if (recv(fd, &c, 1, 0) <= 0)
                return false;
            if (c == '\r')
                continue;
            if (c == '\n')
                break;
            ss << c;
        }
        s = ss.str();
        return true;
    }

    void* response_thread(void* param) {
        server::HttpdInfo *pHttpdInfo = (server::HttpdInfo*)param;
        server *httpd = pHttpdInfo->httpd;
        int msgsock = (int)pHttpdInfo->msgsock;
        std::string address = pHttpdInfo->address;
        std::string port = pHttpdInfo->port;
        int servno = pHttpdInfo->servno;
        std::string str, req, ret;
        std::vector<std::string> vparam;
        std::vector<std::string> vauth;
        std::string res_code;
        std::string res_proto;
        std::string res_msg;
        std::string res_type;
        std::string res_body;
        std::string res_head;
        server::HttpHeader http_headers;
        unsigned long content_length;
        RES_INFO* res_info;
        char buf[BUFSIZ];
        char length[256];
        bool keep_alive;

request_top:
        keep_alive = false;
        res_code.clear();
        res_proto.clear();
        res_msg.clear();
        res_type.clear();
        res_head.clear();
        res_body.clear();
        res_info = NULL;
        http_headers.clear();
        content_length = 0;
        vauth.clear();

        if (!get_line(msgsock, req) || req.empty())
            goto request_end;
        if (VERBOSE(1)) printf("* %s\n", req.c_str());

        do {
            if (!get_line(msgsock, str))
                goto request_end;
            if (str.empty())
                break;
            const char *ptr = str.c_str();

            if (!strnicmp(ptr, "SERVER_", 7) || !strnicmp(ptr, "REMOTE_", 7))
                continue;
            char* stp = (char*)strchr(ptr, ':');
            if (stp) {
                *stp = 0;
                std::string key = ptr;
                std::transform(key.begin(), key.end(), key.begin(), toupper);
                std::string val = trim_string(stp + 1);
                replace_string(key, "-", "_");
                std::transform(key.begin(), key.end(), key.begin(), toupper);
                http_headers[key] = val;
            }
        } while (true);

        if (VERBOSE(2)) {
            server::HttpHeader::const_iterator it;
            for (it = http_headers.begin(); it != http_headers.end(); it++)
                printf("  %s=%s\n", it->first.c_str(), it->second.c_str());
        }

        if (http_headers.count("CONNECTION")
                && !stricmp(http_headers["CONNECTION"].c_str(), "keep-alive"))
            keep_alive = true;

        if (http_headers.count("CONTENT_LENGTH"))
            content_length = atol(http_headers["CONTENT_LENGTH"].c_str());

        if (httpd->loggerfunc) {
            httpd->loggerfunc(pHttpdInfo, req);
        }

        split_string(req, " ", vparam);
        try {
            if (httpd->accept_ips.size() > 0 &&
                    std::find(httpd->accept_ips.begin(), httpd->accept_ips.end(), address)
                    == httpd->accept_ips.end()) {
                res_code = "403";
                res_msg = "Forbidden";
                res_body = "Forbidden";
                goto request_done;
            } else
                if (vparam.size() < 2 || vparam[1][0] != '/') {
                    res_code = "500";
                    res_msg = "Bad Request";
                    res_body = "Bad Request\n";
                    goto request_done;
                } else {
                    if (vparam.size() == 2)
                        res_proto = "HTTP/1.0";
                    else
                        res_proto = vparam[2];
                    std::string auth = http_headers["AUTHORIZATION"];
                    if (!auth.empty()) {
                        if (!strnicmp(auth.c_str(), "basic ", 6))
                            auth = base64_decode(auth.c_str()+6);
                        split_string(auth, ":", vauth);
                    }
                    if (vparam[0] == "GET" || vparam[0] == "POST" || vparam[0] == "HEAD") {
                        std::string root = server::get_realpath(httpd->root + "/");
                        std::string request_uri = vparam[1];
                        std::string script_name = vparam[1];
                        std::string query_string;
                        std::string path_info = "/";
                        size_t end_pos = vparam[1].find_first_of('?');
                        if (end_pos != std::string::npos) {
                            query_string = script_name.substr(end_pos+1);
                            script_name = script_name.substr(0, end_pos);
                            //request_uri = script_name;
                            path_info = script_name;
                        }

                        server::RequestAliases::iterator it_alias;
                        for(it_alias = httpd->request_aliases.begin();
                                it_alias != httpd->request_aliases.end(); it_alias++) {
                            if (path_info == it_alias->first) {
                                vparam[1] = it_alias->second;
                                break;
                            }
                        }

                        std::string before = root;
                        if (before[before.size()-1] == '/')
                            before.resize(before.size() - 1);
                        before += tthttpd::url_decode(script_name);
                        std::string path = server::get_realpath(before);
                        if (before != path && (path.size() < root.size() || path.substr(root.size()) == root)) {
                            if (path.size() > root.size())
                                path = path.c_str() + root.size();
                            else
                                path = "/";
                            res_code = "301";
                            res_msg = "Document Moved";
                            res_body = "Document Moved\n";
                            res_head = "Location: ";
                            res_head += path;
                            res_head += "\n";
                            goto request_done;
                        }
                        /*
                           if (strncmp(root.c_str(), path.c_str(), root.size())) {
                           res_code = "500";
                           res_msg = "Bad Request";
                           res_body = "Bad Request\n";
                           goto request_done;
                           }
                         */

                        std::vector<server::BasicAuthInfo>::iterator it_basicauth;
                        std::vector<std::string> methods;
                        for (it_basicauth = httpd->basic_auths.begin(); it_basicauth != httpd->basic_auths.end(); it_basicauth++) {
                            split_string(it_basicauth->method, "/", methods);
                            if (!methods.empty() && std::find(methods.begin(), methods.end(), vparam[0]) == methods.end()) continue;
                            if (!it_basicauth->target.empty() && strncmp(vparam[1].c_str(), it_basicauth->target.c_str(), it_basicauth->target.size())) continue;
                            break;
                        }
                        if (it_basicauth != httpd->basic_auths.end()) {
                            bool authorized = false;
                            if (!vauth.empty()) {
                                if (VERBOSE(2)) printf("  authorizing %s\n", vparam[1].c_str());
                                std::vector<server::AuthInfo>::iterator it_auth;
                                for (it_auth = it_basicauth->auths.begin(); it_auth != it_basicauth->auths.end(); it_auth++) {
                                    if (it_auth->user != vauth[0]) continue;
                                    /*
                                       std::vector<std::string> pwd = split_string(it_auth->pass, "$");
                                       std::string tmp = vauth[1];
                                       tmp += "$apr1$";
                                       tmp += pwd[2];
                                       std::string pwd_md5 = string_to_hex(crypt(vauth[1].c_str(), pwd[2].c_str()));
                                       printf("%s, %s\n", pwd_md5.c_str(), it_auth->pass.c_str());
                                       if (it_auth->pass != pwd_md5) continue;
                                     */
                                    // TODO: only support plain-text password.
                                    //  hope to access .htpasswd file.
                                    if (it_auth->pass != vauth[1]) continue;
                                    authorized = true;
                                }
                            }
                            if (!authorized) {
                                res_code = "401";
                                res_msg = "Authorization Required";
                                res_head = "WWW-Authenticate: Basic";
                                if (!it_basicauth->realm.empty()) {
                                    res_head += " realm=\"";
                                    res_head += it_basicauth->realm;
                                    res_head += "\"";
                                }
                                res_head += "\r\n";
                                res_body = "Authorization Required";
                                goto request_done;
                            }
                        }
                        if (!vauth.empty()) {
                            server::AcceptAuths::iterator it_accept;
                            for(it_accept = httpd->accept_auths.begin(); it_accept != httpd->accept_auths.end(); it_accept++) {
                                if (!strncmp(it_accept->first.c_str(), script_name.c_str(), it_accept->first.size())) {
                                    if (std::find(
                                                it_accept->second.accept_list.begin(),
                                                it_accept->second.accept_list.end(), vauth[0])
                                            == it_accept->second.accept_list.end()) {
                                        res_code = "401";
                                        res_msg = "Authorization Required";
                                        res_head = "WWW-Authenticate: Basic";
                                        if (!it_basicauth->realm.empty()) {
                                            res_head += " realm=\"";
                                            res_head += it_basicauth->realm;
                                            res_head += "\"";
                                        }
                                        res_head += "\r\n";
                                        res_body = "Authorization Required";
                                        goto request_done;
                                    }
                                }
                            }
                        }

                        if (res_isdir(path) && vparam[1].size() && vparam[1][vparam[1].size()-1] != '/') {
                            res_type = "text/plain";
                            res_code = "301";
                            res_msg = "Document Moved";
                            res_body = "Document Moved\n";
                            res_head = "Location: ";
                            res_head += vparam[1];
                            res_head += "/\n";
                            goto request_done;
                        }

                        server::DefaultPages::iterator it_page;
                        std::string try_path = path;
                        if (try_path[try_path.size()-1] != '/')
                            try_path += "/";
                        for(it_page = httpd->default_pages.begin(); it_page != httpd->default_pages.end(); it_page++) {
                            std::string check_path = try_path + *it_page;
                            if (res_isfile(check_path)) {
                                path = check_path;
                                break;
                            }
                        }

                        server::MimeTypes::iterator it_mime;
                        std::string type;
                        if (!res_isfile(path) && !httpd->default_cgi.empty()) {
                            path = httpd->default_cgi;
                            if (VERBOSE(2)) printf("* running default_cgi: %s\n", path.c_str());
                        }

                        if (httpd->spawn_executable && res_isexe(path, path_info, script_name)) {
                            type = "@";
                        } else {
                            if (!res_iscgi(path, path_info, script_name, httpd->mime_types, type)) {
                                for(it_mime = httpd->mime_types.begin(); it_mime != httpd->mime_types.end(); it_mime++) {
                                    std::string match = ".";
                                    match += it_mime->first;
                                    if (!strcmp(path.c_str()+path.size()-match.size(), match.c_str())) {
                                        type = it_mime->second;
                                        res_type = type;
                                    }
                                    if (!type.empty()) break;
                                }
                            }
                        }

                        if (res_isdir(path)) {
                            if (VERBOSE(2)) printf("  listing %s\n", path.c_str());
                            res_type = "text/html";
                            res_code = "200";
                            res_msg = "OK";
                            if (!httpd->fs_charset.empty()) {
                                res_type += "; charset=";
                                res_type += trim_string(httpd->fs_charset);
                            }
                            res_body = "<html><head><title>";
                            res_body += script_name;
                            res_body += "</title></head><body><h1>";
                            res_body += script_name;
                            res_body += "</h1><hr /><pre>";
                            res_body += "<table border=0>";
                            std::vector<server::ListInfo> flist = res_flist(path);
                            std::vector<server::ListInfo>::iterator it;

                            // TODO: sort and reverse, sort key
                            //std::map<std::string, std::string> params = tthttpd::parse_querystring(query_string);

                            for(it = flist.begin(); it != flist.end(); it++) {
                                std::string name = it->name;
                                res_body += "<tr><td><a href=\"";
                                res_body += tthttpd::url_encode(name);
                                res_body += "\">";
                                res_body += tthttpd::html_encode(name);
                                res_body += "</a></td>";
                                res_body += "<td>";
                                struct tm tm = it->date;
                                sprintf(buf, "%02d-%s-%04d %02d:%02d",
                                        tm.tm_mday,
                                        months[tm.tm_mon],
                                        tm.tm_year+1900,
                                        tm.tm_hour,
                                        tm.tm_min);
                                res_body += buf;
                                res_body += "</td>";
                                res_body += "<td align=right>&nbsp;&nbsp;";
                                if (!it->isdir) {
                                    if (it->size < 1000)
                                        sprintf(buf, "%d", (int)it->size);
                                    else
                                        if (it->size < 1000000)
                                            sprintf(buf, "%dK", (int)it->size/1000);
                                        else
                                            sprintf(buf, "%.1dM", (int)it->size/1000000);
                                    res_body += buf;
                                } else
                                    res_body += "[DIR]";
                                res_body += "</td></tr>";
                            }
                            res_body += "</table></pre ><hr /></body></html>";
                            goto request_done;
                        }

                        res_info = res_fopen(path);
                        if (!res_info) {
                            res_type = "text/plain";
                            res_code = "404";
                            res_msg = "Not Found";
                            res_body = "Not Found\n";
                            goto request_done;
                        }

                        res_code = "200";
                        res_msg = "OK";
                        if (type[0] != '@') {
                            std::string file_time = res_ftime(path);
                            res_info->size = res_fsize(res_info);
                            sprintf(buf, "%d", (int)res_info->size);
                            if (http_headers["IF_MODIFIED_SINCE"] == file_time) {
                                res_close(res_info);
                                res_info = NULL;
                                res_type = "text/plain";
                                res_code = "304";
                                res_msg = "Not Modified";
                                res_body.clear();
                                goto request_done;
                            }
                            if (!type.empty()) {
                                res_head += "Content-Type: ";
                                res_head += type;
                                res_head += ";\r\n";
                            }
                            res_head += "Content-Length: ";
                            res_head += buf;
                            res_head += "\r\n";
                            res_head += "Last-Modified: ";
                            res_head += file_time;
                            res_head += "\r\n";
                            res_head += "Date: ";
                            res_head += res_curtime();
                            res_head += "\r\n";
                            if (http_headers.count("CONNECTION"))
                                res_head += "Connection: " + http_headers["CONNECTION"] + "\r\n";
                        } else {
                            res_close(res_info);
                            res_info = NULL;

                            std::vector<std::string> envs;
                            std::vector<std::string> args;

                            if (type.size() == 1) {
                                args.push_back(path);
                            } else {
                                args.push_back(type.substr(1));
                                args.push_back(path);
                            }
                            if (query_string.size())
                                args.push_back(query_string);

                            std::string env;

                            if (http_headers.count("HTTP_HOST")) {
                                sprintf(buf, "HTTP_HOST=%s:%s", http_headers["HTTP_HOST"].c_str(), httpd->port.c_str());
                                env = buf;
                                envs.push_back(env);
                                http_headers.erase("HTTP_HOST");
                            } else
                                if (httpd->hostname.size()) {
                                    sprintf(buf, "HTTP_HOST=%s:%s", httpd->hostname.c_str(), httpd->port.c_str());
                                    env = buf;
                                    envs.push_back(env);
                                    http_headers.erase("HTTP_HOST");
                                }

                            http_headers.erase("SERVER_PROTOCOL");
                            http_headers.erase("SERVER_ADDR");
                            http_headers.erase("SERVER_NAME");
                            http_headers.erase("REMOTE_ADDR");
                            http_headers.erase("REMOTE_PORT");
                            http_headers.erase("REMOTE_USER");

                            env = "SERVER_PROTOCOL=HTTP/1.1";
                            envs.push_back(env);

                            env = "SERVER_ADDR=";
                            env += httpd->hostaddr[servno];
                            envs.push_back(env);

                            env = "SERVER_NAME=";
                            if (httpd->hostname.size()) {
                                env += httpd->hostname;
                            } else {
                                env += http_headers["HTTP_HOST"];
                            }
                            envs.push_back(env);

                            sprintf(buf, "SERVER_PORT=%s", httpd->port.c_str());
                            env = buf;
                            envs.push_back(env);

                            env = "REMOTE_ADDR=";
                            env += address;
                            envs.push_back(env);

                            sprintf(buf, "REMOTE_PORT=%s", port.c_str());
                            env = buf;
                            envs.push_back(env);

                            if (vauth.size() && !vauth[0].empty()) {
                                env = "REMOTE_USER=";
                                env += vauth[0];
                                envs.push_back(env);
                            }

                            server::HttpHeader::const_iterator it_head;
                            for (it_head = http_headers.begin(); it_head != http_headers.end(); it_head++) {
                                env = "HTTP_";
                                env += it_head->first;
                                env += "=";
                                env += it_head->second;
                                envs.push_back(env);
                            }

                            env = "REQUEST_METHOD=";
                            env += vparam[0];
                            envs.push_back(env);

                            env = "REQUEST_URI=";
                            env += request_uri;
                            envs.push_back(env);

                            env = "SCRIPT_FILENAME=";
                            env += path;
                            envs.push_back(env);

                            env = "SCRIPT_NAME=";
                            env += script_name;
                            envs.push_back(env);

                            env = "QUERY_STRING=";
                            env += query_string;
                            envs.push_back(env);

                            if (!path_info.empty()) {
                                env = "PATH_INFO=";
                                env += path_info;
                                envs.push_back(env);
                            } else {
                                env = "PATH_INFO=";
                                env += request_uri;
                                envs.push_back(env);
                            }

                            env = "REDIRECT_STATUS=1";
                            envs.push_back(env);

                            env = "PATH=";
                            env += getenv("PATH");
                            envs.push_back(env);

#ifdef _WIN32
                            GetWindowsDirectoryA(buf, sizeof(buf));
                            env = "SystemRoot=";
                            env += buf;
                            envs.push_back(env);
#endif

                            char* p = getenv("PERL5LIB");
                            if (p) {
                                env = "PERL5LIB=";
                                env += p;
                                envs.push_back(env);
                            }

                            env = "SERVER_SOFTWARE=tinytinyhttpd";
                            envs.push_back(env);

                            env = "SERVER_PROTOCOL=HTTP/1.1";
                            envs.push_back(env);

                            env = "GATEWAY_INTERFACE=CGI/1.1";
                            envs.push_back(env);

                            server::RequestEnvironments::iterator it_env;
                            for(it_env = httpd->request_environments.begin(); it_env != httpd->request_environments.end(); it_env++) {
                                env = it_env->first + "=";
                                env += it_env->second;
                                envs.push_back(env);
                            }

                            if (vparam[0] == "POST") {
                                env = "CONTENT_TYPE=";
                                env += http_headers["CONTENT_TYPE"];
                                envs.push_back(env);

                                sprintf(buf, "%d", (int)content_length);
                                env = "CONTENT_LENGTH=";
                                env += buf;
                                envs.push_back(env);
                            }

                            if (VERBOSE(4)) {
                                std::vector<std::string>::iterator it;
                                printf("  --- ARGS ---\n");
                                for(it = args.begin(); it != args.end(); it++)
                                    printf("  %s\n", it->c_str());
                                printf("  --- ENVS ---\n");
                                for(it = envs.begin(); it != envs.end(); it++)
                                    printf("  %s\n", it->c_str());
                                printf("  ------------\n");
                            }

                            res_info = res_popen(args, envs);

                            if (res_info && content_length > 0) {
                                while (content_length) {
                                    memset(buf, 0, sizeof(buf));
                                    unsigned long read = recv(msgsock, buf, sizeof(buf), 0);
                                    if (read <= 0) break;
                                    int w = res_write(res_info, buf, read);
                                    content_length -= w;
                                }

                                if (stricmp(http_headers["CONNECTION"].c_str(), "upgrade"))
                                    res_closewriter(res_info);
                                if (content_length) {
                                    res_type = "text/plain";
                                    res_code = "500";
                                    res_msg = "Bad Request";
                                    res_body = "Bad Request\n";
                                    goto request_done;
                                }
                            } else {
                                if (stricmp(http_headers["CONNECTION"].c_str(), "upgrade"))
                                    res_closewriter(res_info);
                            }
                        }
                    } else {
                        res_type = "text/plain";
                        res_code = "500";
                        res_msg = "Bad Request";
                        res_body = "Bad Request\n";
                        goto request_done;
                    }
                }
        }
        catch(...) {
            if (res_code.size() == 0) {
                if (VERBOSE(1)) my_perror(" cached exception");
                res_type = "text/plain";
                res_code = "500";
                res_msg = "Bad Request";
                res_body = "Internal Server Error\n";
            }
        }
request_done:

        if (content_length > 0) {
            while(content_length > 0) {
                int ret = recv(msgsock, buf, sizeof(buf), 0);
                if (ret < 0) {
                    res_type = "text/plain";
                    res_code = "500";
                    res_msg = "Bad Request";
                    res_body = "Bad Request\n";
                }
                content_length -= ret;
            }
        }

        if (res_info && res_info->process) {
            bool res_keep_alive = false;
            res_head.clear();

            do {
                memset(buf, 0, sizeof(buf));
                str = res_fgets(res_info);
                if (str.empty()) break;
                const char *key, *ptr = str.c_str();
                size_t len;
                if (str[0] == '<') {
                    // workaround for broken non-header response.
                    send(msgsock, ptr, strlen(ptr), 0);
                    res_code.clear();
                    break;
                }
                if (VERBOSE(2)) printf("  %s\n", ptr);
                if (res_head.empty()) {
                    if (!strnicmp(ptr, "HTTP/1.", 7)) {
                        char* tmp1;
                        char* tmp2;

                        tmp1 = (char*) strchr(ptr, ' ');
                        if (tmp1) {
                            *tmp1 = 0;
                            res_proto = ptr;
                            tmp2 = strchr(tmp1 + 1, ' ');
                            if (tmp2) {
                                *tmp2 = 0;
                                res_code = tmp1 + 1;
                                res_msg = tmp2 + 1;
                            } else {
                                res_code = tmp1 + 1;
                            }
                        }
                        continue;
                    }
                }
                key = "connection:";
                len = strlen(key);
                if (!strnicmp(ptr, key, len)) {
                    if (!stricmp(trim_string(ptr + len).c_str(), "keep-alive"))
                        res_keep_alive = true;
                }
                key = "WWW-Authenticate: Basic ";
                len = strlen(key);
                if (!strnicmp(ptr, key, len)) {
                    res_code = "401";
                    res_msg = "Unauthorized";
                }
                key = "Status:";
                len = strlen(key);
                if (!strnicmp(ptr, key, len)) {
                    std::vector<std::string> codes;
                    split_string(trim_string(str.substr(len)), " ", codes);
                    if (codes.size())
                        res_code = codes[0];
                    else
                        res_code = "";
                }
                key = "Content-Length:";
                len = strlen(key);
                if (!strnicmp(ptr, key, len)) {
                    res_info->size = (unsigned long)atol(str.substr(len).c_str());
                }
                res_head += ptr;
                res_head += "\r\n";
            } while (true);
            if (!res_keep_alive) {
                keep_alive = false;
                res_head += "Connection: close\r\n";
            }
        }

        if (!res_code.empty()) {
            send(msgsock, res_proto.c_str(), (int)res_proto.size(), 0);
            send(msgsock, " ", 1, 0);
            send(msgsock, res_code.c_str(), (int)res_code.size(), 0);
            send(msgsock, " ", 1, 0);
            send(msgsock, res_msg.c_str(), (int)res_msg.size(), 0);
            send(msgsock, "\r\n", 2, 0);
        }

        if (!res_head.empty()) {
            send(msgsock, res_head.c_str(), (int)res_head.size(), 0);
        }

        if (res_info) {
            send(msgsock, "\r\n", 2, 0);
            unsigned long total = res_info->size;
            int sent = 0;
            if (total != (unsigned long) -1) {
#if defined LINUX_SENDFILE_API
                sent = sendfile(msgsock, res_info->read, NULL, total);
#elif defined FREEBSD_SENDFILE_API
                if (sendfile(msgsock, res_info->read, NULL, total, NULL, NULL, 0) == 0) sent = total;
#elif defined _WIN32
                if (!res_info->process && lpfnTransmitFile && lpfnTransmitFile(
                            msgsock,
                            res_info->read,
                            total,
                            0,
                            NULL,
                            NULL,
                            TF_WRITE_BEHIND)) sent = total;
#endif
            }
            if (sent <= 0) {
                if (VERBOSE(1)) printf("* transfer file using default function\n");
                unsigned int fd = (unsigned int) msgsock;
                fd_set fdset;
                FD_ZERO(&fdset);
                struct timeval tv;
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                while(total != 0) {
                    if (res_info->write) {
                        FD_SET(fd, &fdset);
                        int r = select(FD_SETSIZE, &fdset, NULL, NULL, &tv);
                        if (r < 0) break;
                        if (r > 0 && FD_ISSET(msgsock, &fdset)) {
                            memset(buf, 0, sizeof(buf));
                            int read = recv(msgsock, buf, sizeof(buf), 0);
                            if (read > 0) {
                                res_write(res_info, buf, read);
                            }
                        }
                    }
                    memset(buf, 0, sizeof(buf));
                    long long res = res_read(res_info, buf, sizeof(buf));
                    if (res < 0) break;
                    if (res > 0) {
                        if (VERBOSE(3))
#ifdef _WIN32
                            printf("  reading part %I64d bytes\n", res);
#else
                        printf("  reading part %lld bytes\n", res);
#endif
                        send(msgsock, buf, res, 0);
                        if (total > 0) {
                            total -= res;
                        }
                    } else {
#ifdef _WIN32
                        Sleep(1);
#else
                        usleep(10);
#endif
                    }
                }
            }
            res_close(res_info);
            res_info = NULL;
        } else
            if (!res_body.empty()) {
                if (keep_alive)
                    ret = "Connection: keep-alive\r\n";
                else
                    ret = "Connection: close\r\n";
                send(msgsock, ret.c_str(), (int)ret.size(), 0);

                ret = "Content-Type: ";
                ret += res_type + "\r\n";
                send(msgsock, ret.c_str(), (int)ret.size(), 0);

                ret = res_body;
                sprintf(length, "%u", ret.size());
                ret = "Content-Length: ";
                ret += length;
                ret += "\r\n";
                send(msgsock, ret.c_str(), (int)ret.size(), 0);

                send(msgsock, "\r\n", 2, 0);

                if (vparam.size() > 0 && vparam[0] != "HEAD") {
                    ret = res_body;
                    send(msgsock, ret.c_str(), (int)ret.size(), 0);
                }
            }
            else
                send(msgsock, "\r\n", (int)2, 0);

        if (keep_alive)
            goto request_top;

request_end:
        shutdown(msgsock, SD_BOTH);
        closesocket(msgsock);
        delete pHttpdInfo;
#if defined(_WIN32) && !defined(USE_PTHREAD)
        _endthread();
#else
        pthread_exit(NULL);
#endif

        return NULL;
    }

    void* watch_thread(void* param)
    {
        server *httpd = (server*)param;
        int msgsock;

        int numeric_host = 0;

        // privsep?
        if (!httpd->chroot.empty()) {
            numeric_host = NI_NUMERICHOST;
        }

#ifdef SIGPIPE
        signal(SIGPIPE, SIG_IGN);
#endif

        struct sockaddr *sa;
        struct addrinfo hints;
        struct addrinfo *res, *res0;
        int error;
        const char *hostname;
#ifdef _WIN32
        char on;
#else
        int on;
#endif
        struct timeval timeout;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = httpd->family;
        hints.ai_flags = AI_PASSIVE;
        hints.ai_socktype = SOCK_STREAM;

        if (httpd->hostname.empty())
            hostname = NULL;
        else
            hostname = httpd->hostname.c_str();

        if (hostname == NULL && hints.ai_family == AF_UNSPEC)
            hints.ai_family = AF_INET;

        error = getaddrinfo(hostname, httpd->port.c_str(), &hints, &res);
        if (error) {
            my_perror(gai_strerror(error));
            return NULL;
        }

        res0 = res;

        for ( ; res; res = res->ai_next) {
            int listen_sock;
            unsigned int salen;
            char ntop[NI_MAXHOST], strport[NI_MAXSERV];

            sa = res->ai_addr;
            salen = res->ai_addrlen;

            if (getnameinfo(sa, salen,
                        ntop, sizeof(ntop), strport, sizeof(strport),
                        NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
                fprintf(stderr, "getnameinfo failed\n");
                continue;
            }
            listen_sock = socket(res->ai_family, res->ai_socktype,
                    res->ai_protocol);
            if (listen_sock < 0) {
                fprintf(stderr, "socket: %.100s\n", strerror(errno));
                continue;
            }

            on = 1;
            if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
                        &on, sizeof(on)) == -1)
                fprintf(stderr, "setsockopt SO_REUSEADDR: %s\n", strerror(errno));

            on = 1;
            if (setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY,
                        &on, sizeof(on)) == -1)
                fprintf(stderr, "setsockopt TCP_NODELAY: %s\n", strerror(errno));

            if (bind(listen_sock, sa, salen) < 0) {
                fprintf(stderr, "bind to port %s on %s failed: %.200s.\n",
                        strport, ntop, strerror(errno));
                close(listen_sock);
                continue;
            }

            if (listen(listen_sock, SOMAXCONN) < 0) {
                fprintf(stderr, "listen: %.100s\n", strerror(errno));
                exit(1);
            }

            httpd->socks.push_back(listen_sock);

            if (!(numeric_host == 0)) {
                char address[NI_MAXHOST], port[NI_MAXSERV];
                if (getnameinfo((struct sockaddr*)sa, sizeof(struct sockaddr_storage), address, sizeof(address), port,
                            sizeof(port), numeric_host | NI_NUMERICSERV)) {
                    fprintf(stderr, "could not get hostname\n");
                    continue;
                }
            }
            if (VERBOSE(1)) {
                printf("server started. host: %s port: %s\n", ntop, strport);
            }

            httpd->hostaddr.push_back(ntop);
            // XXX: overwrite
            httpd->port = strport;
#ifdef _WIN32
            if (!lpfnTransmitFile) {
                GUID  guidTransmitFile = WSAID_TRANSMITFILE;
                DWORD dwBytes = 0;
                lpfnTransmitFile = NULL;
                WSAIoctl(listen_sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidTransmitFile, sizeof(GUID), &lpfnTransmitFile, sizeof(LPVOID), &dwBytes, NULL, NULL);
                if (lpfnTransmitFile == NULL)
                    fprintf(stderr, "could not get winsock extension\n");
            }
#endif
        }

        freeaddrinfo(res0);

        int nserver = (int)httpd->socks.size();
        unsigned int maxfd = 0;
        for(int fds = 0; fds < nserver; fds++) {
            if (httpd->socks[fds] > maxfd)
                maxfd = httpd->socks[fds];
        }
        int fdsetsz = howmany(maxfd + 1, NFDBITS) * sizeof(fd_mask);
        fd_set *fdset = (fd_set *)malloc(fdsetsz);

#if HAVE_INET6
        struct sockaddr_storage client;
#else
        char client[sizeof(sockaddr_in)];
#endif
        int client_len = sizeof(client);
        int fds, nfds;
        char address[NI_MAXHOST], port[NI_MAXSERV];

        for(;;) {
            memset(fdset, 0, fdsetsz);

            for(fds = 0; fds < nserver; fds++)
                FD_SET(httpd->socks[fds], fdset);
            nfds = select(maxfd + 1, fdset, NULL, NULL, NULL);
            if (nfds == -1) {
                if (errno == EBADF || errno == EINTR)
                    break;
                my_perror("select");
                continue;
            }
            for(fds = 0; fds < nserver; fds++) {
                int sock = httpd->socks[fds];

                if (!FD_ISSET(sock, &fdset[fds]))
                    continue;

                memset(&client, 0, sizeof(client));
                msgsock = accept(sock, (struct sockaddr *)&client, (socklen_t *)&client_len);
                if (VERBOSE(3)) printf("* accepted socket %d\n", msgsock);
                if (msgsock == -1) {
                    if (errno != EINTR && errno != EWOULDBLOCK)
                        if (VERBOSE(1)) my_perror("accept");
                    closesocket(msgsock);
                    break;
                } else {
                    if (httpd->family == AF_INET) {
                        strcpy(address, inet_ntoa(((struct sockaddr_in *)(void*)&client)->sin_addr));
                    } else {
                        if (getnameinfo((struct sockaddr*)&client, client_len, address, sizeof(address), port,
                                    sizeof(port), numeric_host | NI_NUMERICSERV))
                            fprintf(stderr, "could not get peername\n");
                    }

                    server::HttpdInfo *pHttpdInfo = new server::HttpdInfo;
                    pHttpdInfo->msgsock = msgsock;
                    pHttpdInfo->httpd = httpd;
                    pHttpdInfo->address = address;
                    pHttpdInfo->port = port;
                    pHttpdInfo->servno = fds;

                    on = 1;
                    if (setsockopt(msgsock, IPPROTO_TCP, TCP_NODELAY,
                                &on, sizeof(on)) == -1)
                        fprintf(stderr, "setsockopt TCP_NODELAY: %s\n", strerror(errno));

                    timeout.tv_sec = 3;
                    timeout.tv_usec = 0;
                    if (setsockopt(msgsock, SOL_SOCKET, SO_SNDTIMEO,
                                (char*)&timeout, sizeof(timeout)) == -1)
                        fprintf(stderr, "setsockopt SO_SNDTIMEO: %s\n", strerror(errno));

#if defined(_WIN32) && !defined(USE_PTHREAD)
                    uintptr_t th;
                    while ((int)(th = _beginthread((void (*)(void*))response_thread, 0, (void*)pHttpdInfo)) == -1) {
                        Sleep(1);
                    }
#else
                    pthread_t pth;
                    while (pthread_create(&pth, NULL, response_thread, (void*)pHttpdInfo) != 0) {
                        usleep(100);
                    }
                    pthread_detach(pth);
#endif
                }
            }
        }

        delete[] fdset;

#if defined(_WIN32) && !defined(USE_PTHREAD)
        _endthread();
#else
        pthread_exit(NULL);
#endif

        return NULL;
    }

    bool server::start() {
#ifndef _WIN32
        set_priv(user.c_str(), chroot.c_str(), "tthttpd");
#endif
        if (thread)
            return false;
#if defined(_WIN32) && !defined(USE_PTHREAD)
        thread = (HANDLE)_beginthread((void (*)(void*))watch_thread, 0, (void*)this);
#else
        pthread_create(&thread, NULL, watch_thread, (void*)this);
#endif
        return thread ? true : false;
    }

    bool server::stop() {
        if (!thread)
            return false;
        if (verbose_mode >= 1)
            printf("exiting...\n");
        for(std::vector<unsigned int>::iterator sock = socks.begin(); sock != socks.end(); sock++){
            shutdown(*sock, SD_BOTH);
            closesocket(*sock);
        }
#if defined(_WIN32) && !defined(USE_PTHREAD)
        TerminateThread(thread, 0);
#else
        pthread_kill(thread, SIGINT);
#endif
        wait();
        return true;
    }

    bool server::wait() {
#if defined(_WIN32) && !defined(USE_PTHREAD)
        WaitForSingleObject(thread, INFINITE);
#else
        pthread_join(thread, NULL);
#endif
        thread = NULL;
        return true;
    }

    bool server::is_running() {
        return thread ? true : false;
    }

}

// vim:set et:
