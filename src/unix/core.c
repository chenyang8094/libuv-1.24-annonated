/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stddef.h> /* NULL */
#include <stdio.h> /* printf */
#include <stdlib.h>
#include <string.h> /* strerror */
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <limits.h> /* INT_MAX, PATH_MAX, IOV_MAX */
#include <sys/uio.h> /* writev */
#include <sys/resource.h> /* getrusage */
#include <pwd.h>
#include <sched.h>

#ifdef __sun
# include <netdb.h> /* MAXHOSTNAMELEN on Solaris */
# include <sys/filio.h>
# include <sys/types.h>
# include <sys/wait.h>
#endif

#ifdef __APPLE__
# include <mach-o/dyld.h> /* _NSGetExecutablePath */
# include <sys/filio.h>
# if defined(O_CLOEXEC)
#  define UV__O_CLOEXEC O_CLOEXEC
# endif
#endif

#if defined(__DragonFly__)      || \
    defined(__FreeBSD__)        || \
    defined(__FreeBSD_kernel__) || \
    defined(__NetBSD__)
# include <sys/sysctl.h>
# include <sys/filio.h>
# include <sys/wait.h>
# include <sys/param.h>
# include <sys/cpuset.h>
# define UV__O_CLOEXEC O_CLOEXEC
# if defined(__FreeBSD__)
#  define uv__accept4 accept4
# endif
# if defined(__NetBSD__)
#  define uv__accept4(a, b, c, d) paccept((a), (b), (c), NULL, (d))
# endif
# if (defined(__FreeBSD__) && __FreeBSD__ >= 10) || defined(__NetBSD__)
#  define UV__SOCK_NONBLOCK SOCK_NONBLOCK
#  define UV__SOCK_CLOEXEC  SOCK_CLOEXEC
# endif
#endif

#if defined(__ANDROID_API__) && __ANDROID_API__ < 21
# include <dlfcn.h>  /* for dlsym */
#endif

#if defined(__MVS__)
#include <sys/ioctl.h>
#endif

#if !defined(__MVS__)
#include <sys/param.h> /* MAXHOSTNAMELEN on Linux and the BSDs */
#endif

/* Fallback for the maximum hostname length */
#ifndef MAXHOSTNAMELEN
# define MAXHOSTNAMELEN 256
#endif

static int uv__run_pending(uv_loop_t* loop);


uint64_t uv_hrtime(void) {
  return uv__hrtime(UV_CLOCK_PRECISE);
}

/*  */
void uv_close(uv_handle_t* handle, uv_close_cb close_cb) {
  /*  */
  assert(!uv__is_closing(handle));

  /*  */
  handle->flags |= UV_HANDLE_CLOSING;
  /*  */
  handle->close_cb = close_cb;

  /*  */
  switch (handle->type) {
  /*  */
  case UV_NAMED_PIPE:
    uv__pipe_close((uv_pipe_t*)handle);
    break;

  /*  */
  case UV_TTY:
    uv__stream_close((uv_stream_t*)handle);
    break;

  /*  */
  case UV_TCP:
    uv__tcp_close((uv_tcp_t*)handle);
    break;

  /*  */
  case UV_UDP:
    uv__udp_close((uv_udp_t*)handle);
    break;

  /*  */
  case UV_PREPARE:
    uv__prepare_close((uv_prepare_t*)handle);
    break;

  /*  */
  case UV_CHECK:
    uv__check_close((uv_check_t*)handle);
    break;

  /*  */
  case UV_IDLE:
    uv__idle_close((uv_idle_t*)handle);
    break;

  /*  */
  case UV_ASYNC:
    uv__async_close((uv_async_t*)handle);
    break;

  /*  */
  case UV_TIMER:
    uv__timer_close((uv_timer_t*)handle);
    break;

  /*  */
  case UV_PROCESS:
    uv__process_close((uv_process_t*)handle);
    break;

  /*  */
  case UV_FS_EVENT:
    uv__fs_event_close((uv_fs_event_t*)handle);
    break;

  /*  */
  case UV_POLL:
    uv__poll_close((uv_poll_t*)handle);
    break;

  /*  */
  case UV_FS_POLL:
    uv__fs_poll_close((uv_fs_poll_t*)handle);
    break;

  /*  */
  case UV_SIGNAL:
    uv__signal_close((uv_signal_t*) handle);
    /* Signal handles may not be closed immediately. The signal code will
     * itself close uv__make_close_pending whenever appropriate. */
    return;

  default:
    assert(0);
  }

  uv__make_close_pending(handle);
}

/*  */
int uv__socket_sockopt(uv_handle_t* handle, int optname, int* value) {
  int r;
  int fd;
  socklen_t len;

  if (handle == NULL || value == NULL)
    return UV_EINVAL;

  /*  */
  if (handle->type == UV_TCP || handle->type == UV_NAMED_PIPE)
    fd = uv__stream_fd((uv_stream_t*) handle);
  else if (handle->type == UV_UDP) /*  */
    fd = ((uv_udp_t *) handle)->io_watcher.fd;
  else
    return UV_ENOTSUP;

  len = sizeof(*value);

  /*  */
  if (*value == 0)
    r = getsockopt(fd, SOL_SOCKET, optname, value, &len);
  else/*  */
    r = setsockopt(fd, SOL_SOCKET, optname, (const void*) value, len);

  if (r < 0)
    return UV__ERR(errno);

  return 0;
}

/*  */
void uv__make_close_pending(uv_handle_t* handle) {
  /*  */
  assert(handle->flags & UV_HANDLE_CLOSING);
  /*  */
  assert(!(handle->flags & UV_HANDLE_CLOSED));
  /*  */
  handle->next_closing = handle->loop->closing_handles;
  /*  */
  handle->loop->closing_handles = handle;
}

int uv__getiovmax(void) {
#if defined(IOV_MAX)
  return IOV_MAX;
#elif defined(_SC_IOV_MAX)
  static int iovmax = -1;
  if (iovmax == -1) {
    iovmax = sysconf(_SC_IOV_MAX);
    /* On some embedded devices (arm-linux-uclibc based ip camera),
     * sysconf(_SC_IOV_MAX) can not get the correct value. The return
     * value is -1 and the errno is EINPROGRESS. Degrade the value to 1.
     */
    if (iovmax == -1) iovmax = 1;
  }
  return iovmax;
#else
  return 1024;
#endif
}

/*  */
static void uv__finish_close(uv_handle_t* handle) {
  /* Note: while the handle is in the UV_HANDLE_CLOSING state now, it's still
   * possible for it to be active in the sense that uv__is_active() returns
   * true.
   *
   * A good example is when the user calls uv_shutdown(), immediately followed
   * by uv_close(). The handle is considered active at this point because the
   * completion of the shutdown req is still pending.
   */
  assert(handle->flags & UV_HANDLE_CLOSING);
  assert(!(handle->flags & UV_HANDLE_CLOSED));
  handle->flags |= UV_HANDLE_CLOSED;

  switch (handle->type) {
    case UV_PREPARE:
    case UV_CHECK:
    case UV_IDLE:
    case UV_ASYNC:
    case UV_TIMER:
    case UV_PROCESS:
    case UV_FS_EVENT:
    case UV_FS_POLL:
    case UV_POLL:
    case UV_SIGNAL:
      break;

    case UV_NAMED_PIPE:
    case UV_TCP:
    case UV_TTY:/*  */
      uv__stream_destroy((uv_stream_t*)handle);
      break;

    case UV_UDP:/*  */
      uv__udp_finish_close((uv_udp_t*)handle);
      break;

    default:
      assert(0);
      break;
  }
   
  /*  */
  uv__handle_unref(handle);
  /*  */
  QUEUE_REMOVE(&handle->handle_queue);

  /*  */
  if (handle->close_cb) {
    handle->close_cb(handle);
  }
}

/*  */
static void uv__run_closing_handles(uv_loop_t* loop) {
  uv_handle_t* p;
  uv_handle_t* q;

  /*  */
  p = loop->closing_handles;
  /*  */
  loop->closing_handles = NULL;

  /*  */
  while (p) {
    q = p->next_closing;
    uv__finish_close(p);
    p = q;
  }
}


/*  */
int uv_is_closing(const uv_handle_t* handle) {
  return uv__is_closing(handle);
}


/*  */
uv_os_fd_t uv_backend_fd(const uv_loop_t* loop) {
  return loop->backend_fd;
}

/* 计算后端（在linux上就是epoll_wait）超时时间 */
int uv_backend_timeout(const uv_loop_t* loop) {
  /* 如果loop即将停止（uv_stop() 已在之前被调用），那么超时将会是 0 */
  if (loop->stop_flag != 0)
    return 0;

  /* 如果loop内没有激活的句柄和请求，那么超时将会是 0 */
  if (!uv__has_active_handles(loop) && !uv__has_active_reqs(loop))
    return 0;

  /* 如果loop内有激活的闲置句柄，那么超时将会是 0  */
  if (!QUEUE_EMPTY(&loop->idle_handles))
    return 0;

  /* 如果有待处理的悬挂watcher,那么超时将会是 0    */
  if (!QUEUE_EMPTY(&loop->pending_queue))
    return 0;

  /* 如果有正在等待被关闭的句柄，那么超时将会是 0  */
  if (loop->closing_handles)
    return 0;

  /* 否则获取下一个超时时间（即能保证至少有一个定时器超时） */
  return uv__next_timeout(loop);
}

/* 判断一个loop是否还是激活状态,激活返回1，否则返回0 */
static int uv__loop_alive(const uv_loop_t* loop) {
  /* 有正在活动的handles 或 有正在活动的请求待处理 或 有正在被关闭的handles */
  return uv__has_active_handles(loop) ||
         uv__has_active_reqs(loop) ||
         loop->closing_handles != NULL;
}

/*  */
int uv_loop_alive(const uv_loop_t* loop) {
    return uv__loop_alive(loop);
}

/* 开始loop循环 */
int uv_run(uv_loop_t* loop, uv_run_mode mode) {
  int timeout;
  int r;
  int ran_pending;

  /* 判断一个loop是否还是激活状态 */
  r = uv__loop_alive(loop);
  /* 非激活为什么要更新时间 TODO */
  if (!r)
    uv__update_time(loop);

  /* 当loop为激活状态且stop_flag为0 */
  while (r != 0 && loop->stop_flag == 0) {
    /* 更新loop->time = uv__hrtime(UV_CLOCK_FAST) / 1000000; */
    uv__update_time(loop);
    /* 执行定时器，凡是定时器的timeout小于loop->time的此时都会被执行 */
    uv__run_timers(loop);
    /* 执行所有被悬挂的watcher，正常情况下，所有的 I/O watcher都会在轮询 I/O 
    后立刻被调用。但是有些情况下，回调可能会被推迟至下一次循环迭代中再执行。
    任何上一次循环中被推迟的回调，都将在这个时候得到执行。返回值为0表示没有
    悬挂的watcher回调被执行，为1表示至少有一个悬挂的watcher回调被执行 */
    ran_pending = uv__run_pending(loop);
    /* 执行空闲handle回调，这个函数使用宏定义，参见loop-watcher.c文件 */
    uv__run_idle(loop);
    /* 执行预备handle回调，这个函数使用宏定义，参见loop-watcher.c文件*/
    uv__run_prepare(loop);

    /* 超时时间默认为0，为不阻塞 */
    timeout = 0;
    /* UV_RUN_ONCE表示在loop退出之前至少要执行一次回调操作 */
    if ((mode == UV_RUN_ONCE && !ran_pending) || mode == UV_RUN_DEFAULT){
      /* 计算后端（在linux上就是epoll_wait）超时时间 */
      timeout = uv_backend_timeout(loop);
    }
      
    /* 进行io事件轮询 */
    uv__io_poll(loop, timeout);
    /* 执行检查handle回调 ，这个函数使用宏定义，参见loop-watcher.c文件*/
    uv__run_check(loop);
    /* 执行关闭回调 */
    uv__run_closing_handles(loop);

    /* UV_RUN_ONCE要求loop返回之前至少执行一次回调，所以需要特殊处理一下 */
    if (mode == UV_RUN_ONCE) {
      /* UV_RUN_ONCE implies forward progress: at least one callback must have
       * been invoked when it returns. uv__io_poll() can return without doing
       * I/O (meaning: no callbacks) when its timeout expires - which means we
       * have pending timers that satisfy the forward progress constraint.
       *
       * UV_RUN_NOWAIT makes no guarantees about progress so it's omitted from
       * the check.
       */
      /* 更新当前时间 */
      uv__update_time(loop);
      /* 运行定时器 */
      uv__run_timers(loop);
    }

    /* 再次检查loop激活状态 */
    r = uv__loop_alive(loop);
    /* 如果是UV_RUN_ONCE或UV_RUN_NOWAIT，则退出循环 */
    if (mode == UV_RUN_ONCE || mode == UV_RUN_NOWAIT)
      break;
  }

  /* The if statement lets gcc compile it to a conditional store. Avoids
   * dirtying a cache line.
   * 
   *  TODO:
   */
  if (loop->stop_flag != 0)
    loop->stop_flag = 0;

  return r;
}

/* 更新loop时间 */
void uv_update_time(uv_loop_t* loop) {
  uv__update_time(loop);
}

/* 判断一个handle是否是激活状态 */
int uv_is_active(const uv_handle_t* handle) {
  return uv__is_active(handle);
}

/* Open a socket in non-blocking close-on-exec mode, atomically if possible. */
int uv__socket(int domain, int type, int protocol) {
  int sockfd;
  int err;

#if defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
  /* 
   如果socket调用支持设置SOCK_NONBLOCK和SOCK_CLOEXEC标志
   原型：int socket(int domain, int type, int protocol);
   详见：http://man7.org/linux/man-pages/man2/socket.2.html
   */
  sockfd = socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
  /* 创建成功就直接返回 */
  if (sockfd != -1)
    return sockfd;

  /* 如果错误不是因为Invalid flags */
  if (errno != EINVAL)
    return UV__ERR(errno);
#endif

  /*  
   运行到这里，说明不支持直接设置flag,用老的方式创建
   */
  sockfd = socket(domain, type, protocol);
  if (sockfd == -1)
    return UV__ERR(errno);

  /* 单独设置非阻塞模式和CLOEXEC */
  err = uv__nonblock(sockfd, 1);
  if (err == 0)
    err = uv__cloexec(sockfd, 1);

  /* 错误返回错误 */
  if (err) {
    uv__close(sockfd);
    return err;
  }

#if defined(SO_NOSIGPIPE)
  {
    int on = 1;
    /* 
       SO_NOSIGPIPE是bsd平台的一个flag，用于设置是否忽略SIGPIPE信号（对已崩溃的对端连续两次两次将产生SIGPIPE）。
       https://www.freebsd.org/cgi/man.cgi?query=setsockopt&sektion=2&manpath=freebsd-release-ports
       这个标志不具有可移植性，更好的做法是在send的时候加上MSG_NOSIGNAL，详见：https://linux.die.net/man/2/send
       对于setsockopt，详见：https://linux.die.net/man/2/setsockopt
     */
    setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  }
#endif

  return sockfd;
}

/* get a file pointer to a file in read-only and close-on-exec mode */
FILE* uv__open_file(const char* path) {
  int fd;
  FILE* fp;

  fd = uv__open_cloexec(path, O_RDONLY);
  if (fd < 0)
    return NULL;

   fp = fdopen(fd, "r");
   if (fp == NULL)
     uv__close(fd);

   return fp;
}


int uv__accept(int sockfd) {
  int peerfd;
  int err;

  assert(sockfd >= 0);

  while (1) {
#if defined(__linux__)                          || \
    (defined(__FreeBSD__) && __FreeBSD__ >= 10) || \
    defined(__NetBSD__)
    static int no_accept4;

    if (no_accept4)
      goto skip;

    peerfd = uv__accept4(sockfd,
                         NULL,
                         NULL,
                         UV__SOCK_NONBLOCK|UV__SOCK_CLOEXEC);
    if (peerfd != -1)
      return peerfd;

    if (errno == EINTR)
      continue;

    if (errno != ENOSYS)
      return UV__ERR(errno);

    no_accept4 = 1;
skip:
#endif

    peerfd = accept(sockfd, NULL, NULL);
    if (peerfd == -1) {
      if (errno == EINTR)
        continue;
      return UV__ERR(errno);
    }

    err = uv__cloexec(peerfd, 1);
    if (err == 0)
      err = uv__nonblock(peerfd, 1);

    if (err) {
      uv__close(peerfd);
      return err;
    }

    return peerfd;
  }
}


int uv__close_nocheckstdio(int fd) {
  int saved_errno;
  int rc;

  assert(fd > -1);  /* Catch uninitialized io_watcher.fd bugs. */

  saved_errno = errno;
  rc = close(fd);
  if (rc == -1) {
    rc = UV__ERR(errno);
    if (rc == UV_EINTR || rc == UV__ERR(EINPROGRESS))
      rc = 0;    /* The close is in progress, not an error. */
    errno = saved_errno;
  }

  return rc;
}


int uv__close(int fd) {
  assert(fd > STDERR_FILENO);  /* Catch stdio close bugs. */
#if defined(__MVS__)
  SAVE_ERRNO(epoll_file_close(fd));
#endif
  return uv__close_nocheckstdio(fd);
}


int uv__nonblock_ioctl(int fd, int set) {
  int r;

  do
    r = ioctl(fd, FIONBIO, &set);
  while (r == -1 && errno == EINTR);

  if (r)
    return UV__ERR(errno);

  return 0;
}


#if !defined(__CYGWIN__) && !defined(__MSYS__)
int uv__cloexec_ioctl(int fd, int set) {
  int r;

  do
    r = ioctl(fd, set ? FIOCLEX : FIONCLEX);
  while (r == -1 && errno == EINTR);

  if (r)
    return UV__ERR(errno);

  return 0;
}
#endif


int uv__nonblock_fcntl(int fd, int set) {
  int flags;
  int r;

  do
    r = fcntl(fd, F_GETFL);
  while (r == -1 && errno == EINTR);

  if (r == -1)
    return UV__ERR(errno);

  /* Bail out now if already set/clear. */
  if (!!(r & O_NONBLOCK) == !!set)
    return 0;

  if (set)
    flags = r | O_NONBLOCK;
  else
    flags = r & ~O_NONBLOCK;

  do
    r = fcntl(fd, F_SETFL, flags);
  while (r == -1 && errno == EINTR);

  if (r)
    return UV__ERR(errno);

  return 0;
}


int uv__cloexec_fcntl(int fd, int set) {
  int flags;
  int r;

  do
    r = fcntl(fd, F_GETFD);
  while (r == -1 && errno == EINTR);

  if (r == -1)
    return UV__ERR(errno);

  /* Bail out now if already set/clear. */
  if (!!(r & FD_CLOEXEC) == !!set)
    return 0;

  if (set)
    flags = r | FD_CLOEXEC;
  else
    flags = r & ~FD_CLOEXEC;

  do
    r = fcntl(fd, F_SETFD, flags);
  while (r == -1 && errno == EINTR);

  if (r)
    return UV__ERR(errno);

  return 0;
}


ssize_t uv__recvmsg(int fd, struct msghdr* msg, int flags) {
  struct cmsghdr* cmsg;
  ssize_t rc;
  int* pfd;
  int* end;
#if defined(__linux__)
  static int no_msg_cmsg_cloexec;
  if (no_msg_cmsg_cloexec == 0) {
    rc = recvmsg(fd, msg, flags | 0x40000000);  /* MSG_CMSG_CLOEXEC */
    if (rc != -1)
      return rc;
    if (errno != EINVAL)
      return UV__ERR(errno);
    rc = recvmsg(fd, msg, flags);
    if (rc == -1)
      return UV__ERR(errno);
    no_msg_cmsg_cloexec = 1;
  } else {
    rc = recvmsg(fd, msg, flags);
  }
#else
  rc = recvmsg(fd, msg, flags);
#endif
  if (rc == -1)
    return UV__ERR(errno);
  if (msg->msg_controllen == 0)
    return rc;
  for (cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL; cmsg = CMSG_NXTHDR(msg, cmsg))
    if (cmsg->cmsg_type == SCM_RIGHTS)
      for (pfd = (int*) CMSG_DATA(cmsg),
           end = (int*) ((char*) cmsg + cmsg->cmsg_len);
           pfd < end;
           pfd += 1)
        uv__cloexec(*pfd, 1);
  return rc;
}


int uv_cwd(char* buffer, size_t* size) {
  if (buffer == NULL || size == NULL)
    return UV_EINVAL;

  if (getcwd(buffer, *size) == NULL)
    return UV__ERR(errno);

  *size = strlen(buffer);
  if (*size > 1 && buffer[*size - 1] == '/') {
    buffer[*size-1] = '\0';
    (*size)--;
  }

  return 0;
}


int uv_chdir(const char* dir) {
  if (chdir(dir))
    return UV__ERR(errno);

  return 0;
}


void uv_disable_stdio_inheritance(void) {
  int fd;

  /* Set the CLOEXEC flag on all open descriptors. Unconditionally try the
   * first 16 file descriptors. After that, bail out after the first error.
   */
  for (fd = 0; ; fd++)
    if (uv__cloexec(fd, 1) && fd > 15)
      break;
}


int uv_fileno(const uv_handle_t* handle, uv_os_fd_t* fd) {
  int fd_out;

  switch (handle->type) {
  case UV_TCP:
  case UV_NAMED_PIPE:
  case UV_TTY:
    fd_out = uv__stream_fd((uv_stream_t*) handle);
    break;

  case UV_UDP:
    fd_out = ((uv_udp_t *) handle)->io_watcher.fd;
    break;

  case UV_POLL:
    fd_out = ((uv_poll_t *) handle)->io_watcher.fd;
    break;

  default:
    return UV_EINVAL;
  }

  if (uv__is_closing(handle) || fd_out == -1)
    return UV_EBADF;

  *fd = fd_out;
  return 0;
}

/* 执行所有悬挂中的回调 */
static int uv__run_pending(uv_loop_t* loop) {
  QUEUE* q;
  QUEUE pq;
  uv__io_t* w;

  /* loop->pending_queue如果为空，表示没有悬挂的watcher要处理 */
  if (QUEUE_EMPTY(&loop->pending_queue))
    return 0;

  /* 将队列loop->pending_queue移动到pq中 */
  QUEUE_MOVE(&loop->pending_queue, &pq);
  
  /* 遍历pq队列 */
  while (!QUEUE_EMPTY(&pq)) {
    /* 接下来三步是标准的从队列中取一个几点的操作 */
    q = QUEUE_HEAD(&pq);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);
    /* 根据q还原uv__io_t架构，典型的container_of用法 */
    w = QUEUE_DATA(q, uv__io_t, pending_queue);
    /* 回调这个watcher的回调函数 */
    w->cb(loop, w, POLLOUT);
  }

  return 1;
}

/* 返回大于等于val的最小的数，并且该数是2的次幂，如val是513，则返回1024 */
static unsigned int next_power_of_two(unsigned int val) {
  val -= 1;
  val |= val >> 1;
  val |= val >> 2;
  val |= val >> 4;
  val |= val >> 8;
  val |= val >> 16;
  val += 1;
  return val;
}

/* 重新调整loop->watchers大小 */
static void maybe_resize(uv_loop_t* loop, unsigned int len) {
  /* 可以看成数组的形式 uv__io_t * watchers[] */
  uv__io_t** watchers;
  void* fake_watcher_list;
  void* fake_watcher_count;
  unsigned int nwatchers;
  unsigned int i;

  /* 无需调整 */
  if (len <= loop->nwatchers)
    return;

  /* Preserve fake watcher list and count at the end of the watchers */
  if (loop->watchers != NULL) {
    /* loop->watchers数组的最后两项分别保存着每次epoll_wait后events和nfds */
    fake_watcher_list = loop->watchers[loop->nwatchers];
    fake_watcher_count = loop->watchers[loop->nwatchers + 1];
  } else {
    fake_watcher_list = NULL;
    fake_watcher_count = NULL;
  }

  /* 计算需要的长度 */
  nwatchers = next_power_of_two(len + 2) - 2;
  /* 按照新的大小重新分配空间(还多分配了2个) */
  watchers = uv__realloc(loop->watchers,
                         (nwatchers + 2) * sizeof(loop->watchers[0]));

  /* uv__realloc失败，直接abort */
  if (watchers == NULL)
    abort();
  /* 情况每一项 */
  for (i = loop->nwatchers; i < nwatchers; i++)
    watchers[i] = NULL;
  /* 这里利用多分配的那2个数组元素，保存events和nfds*/
  watchers[nwatchers] = fake_watcher_list;
  watchers[nwatchers + 1] = fake_watcher_count;

  /* 指向新的watchers */
  loop->watchers = watchers;
  /* 新的watchers数组大小 */
  loop->nwatchers = nwatchers;
}

/* 初始化一个io watcher */
void uv__io_init(uv__io_t* w, uv__io_cb cb, int fd) {
  assert(cb != NULL);
  assert(fd >= -1);
  /* 初始化悬挂队列 */
  QUEUE_INIT(&w->pending_queue);
  /* 初始化watcher队列 */
  QUEUE_INIT(&w->watcher_queue);
  /* 对应的回调函数 */
  w->cb = cb;
  /* 绑定的fd */
  w->fd = fd;
  /* 已注册监听事件 */
  w->events = 0;
  /* 待注册监听事件 */
  w->pevents = 0;

#if defined(UV_HAVE_KQUEUE)
  w->rcount = 0;
  w->wcount = 0;
#endif /* defined(UV_HAVE_KQUEUE) */
}

/* 向loop注册一个io watcher，其关注的事件为events */
void uv__io_start(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  /*
     只允许watcher关注POLLIN、POLLOUT、UV__POLLRDHUP、UV__POLLPRI子集
     更多的事件可参见：http://man7.org/linux/man-pages/man2/epoll_ctl.2.html
  */
  assert(0 == (events & ~(POLLIN | POLLOUT | UV__POLLRDHUP | UV__POLLPRI)));
  /* 要注册的事件不能为空 */
  assert(0 != events);
  /* watcher绑定的fd必须合法 */
  assert(w->fd >= 0);
  assert(w->fd < INT_MAX);

  /* 设置watcher悬挂的事件pending events */
  w->pevents |= events;
  /* 重新调整loop->watchers大小 */
  maybe_resize(loop, w->fd + 1);

#if !defined(__sun)
  /* The event ports backend needs to rearm all file descriptors on each and
   * every tick of the event loop but the other backends allow us to
   * short-circuit here if the event mask is unchanged.
   */
  if (w->events == w->pevents)
    return;
#endif

  /* 如果该watcher还没有被加入到watcher队列中，就将其加入loop->watcher_queue */
  if (QUEUE_EMPTY(&w->watcher_queue))
    QUEUE_INSERT_TAIL(&loop->watcher_queue, &w->watcher_queue);

  /* 如果该fd对应的loop->watchers数组项还为空（意思是之前没有在该fd上注册过watcher，即一个fd上存在多个watcher） */
  if (loop->watchers[w->fd] == NULL) {
    /* 将该watcher加入fd下标对应的loop->watchers数组项 */
    loop->watchers[w->fd] = w;
    /* nfds表示该loop上注册的fd数目 */
    loop->nfds++;
  }
}

/* 停止一个io watcher对events的监听 */
void uv__io_stop(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  assert(0 == (events & ~(POLLIN | POLLOUT | UV__POLLRDHUP | UV__POLLPRI)));
  assert(0 != events);

  if (w->fd == -1)
    return;

  assert(w->fd >= 0);

  /* Happens when uv__io_stop() is called on a handle that was never started. */
  if ((unsigned) w->fd >= loop->nwatchers)
    return;

  /* 如果pevents和events完全相同，那么相交之后pevents为0 */
  w->pevents &= ~events;

  /* pevents为0说明该watcher没有pending事件了 */
  if (w->pevents == 0) {
    /* 以下两步将该watcher移除watcher_queue */
    QUEUE_REMOVE(&w->watcher_queue);
    QUEUE_INIT(&w->watcher_queue);
  
    /* 如果loop->watchers[w->fd]不为空 */
    if (loop->watchers[w->fd] != NULL) {
      /* 则该数组项上的watcher必须是该watcher */
      assert(loop->watchers[w->fd] == w);
      assert(loop->nfds > 0);
      /* 清空loop->watchers[w->fd]数组项 */
      loop->watchers[w->fd] = NULL;
      /* loop上注册的fd减一 */
      loop->nfds--;
      /* 清空watcher注册的事件 */
      w->events = 0;
    }
  }/* 否则，该watcher上还有pending事件要处理 */
  else if (QUEUE_EMPTY(&w->watcher_queue)){
    /* 将该watcher加入loop->watcher_queue */
    QUEUE_INSERT_TAIL(&loop->watcher_queue, &w->watcher_queue);
  }
}


void uv__io_close(uv_loop_t* loop, uv__io_t* w) {
  uv__io_stop(loop, w, POLLIN | POLLOUT | UV__POLLRDHUP | UV__POLLPRI);
  QUEUE_REMOVE(&w->pending_queue);

  /* Remove stale events for this file descriptor */
  uv__platform_invalidate_fd(loop, w->fd);
}

/*  */
void uv__io_feed(uv_loop_t* loop, uv__io_t* w) {
  /* 如果该watcher还未加入pending_queue，就将其加入loop->pending_queue */
  if (QUEUE_EMPTY(&w->pending_queue))
    QUEUE_INSERT_TAIL(&loop->pending_queue, &w->pending_queue);
}

/*  */
int uv__io_active(const uv__io_t* w, unsigned int events) {
  /*  */
  assert(0 == (events & ~(POLLIN | POLLOUT | UV__POLLRDHUP | UV__POLLPRI)));
  assert(0 != events);
  /*  */
  return 0 != (w->pevents & events);
}

/*  */
int uv__fd_exists(uv_loop_t* loop, int fd) {
  /*  */
  return (unsigned) fd < loop->nwatchers && loop->watchers[fd] != NULL;
}


int uv_getrusage(uv_rusage_t* rusage) {
  struct rusage usage;

  if (getrusage(RUSAGE_SELF, &usage))
    return UV__ERR(errno);

  rusage->ru_utime.tv_sec = usage.ru_utime.tv_sec;
  rusage->ru_utime.tv_usec = usage.ru_utime.tv_usec;

  rusage->ru_stime.tv_sec = usage.ru_stime.tv_sec;
  rusage->ru_stime.tv_usec = usage.ru_stime.tv_usec;

#if !defined(__MVS__)
  rusage->ru_maxrss = usage.ru_maxrss;
  rusage->ru_ixrss = usage.ru_ixrss;
  rusage->ru_idrss = usage.ru_idrss;
  rusage->ru_isrss = usage.ru_isrss;
  rusage->ru_minflt = usage.ru_minflt;
  rusage->ru_majflt = usage.ru_majflt;
  rusage->ru_nswap = usage.ru_nswap;
  rusage->ru_inblock = usage.ru_inblock;
  rusage->ru_oublock = usage.ru_oublock;
  rusage->ru_msgsnd = usage.ru_msgsnd;
  rusage->ru_msgrcv = usage.ru_msgrcv;
  rusage->ru_nsignals = usage.ru_nsignals;
  rusage->ru_nvcsw = usage.ru_nvcsw;
  rusage->ru_nivcsw = usage.ru_nivcsw;
#endif

  return 0;
}


int uv__open_cloexec(const char* path, int flags) {
  int err;
  int fd;

#if defined(UV__O_CLOEXEC)
  static int no_cloexec;

  if (!no_cloexec) {
    fd = open(path, flags | UV__O_CLOEXEC);
    if (fd != -1)
      return fd;

    if (errno != EINVAL)
      return UV__ERR(errno);

    /* O_CLOEXEC not supported. */
    no_cloexec = 1;
  }
#endif

  fd = open(path, flags);
  if (fd == -1)
    return UV__ERR(errno);

  err = uv__cloexec(fd, 1);
  if (err) {
    uv__close(fd);
    return err;
  }

  return fd;
}


int uv__dup2_cloexec(int oldfd, int newfd) {
  int r;
#if (defined(__FreeBSD__) && __FreeBSD__ >= 10) || defined(__NetBSD__)
  r = dup3(oldfd, newfd, O_CLOEXEC);
  if (r == -1)
    return UV__ERR(errno);
  return r;
#elif defined(__FreeBSD__) && defined(F_DUP2FD_CLOEXEC)
  r = fcntl(oldfd, F_DUP2FD_CLOEXEC, newfd);
  if (r != -1)
    return r;
  if (errno != EINVAL)
    return UV__ERR(errno);
  /* Fall through. */
#elif defined(__linux__)
  static int no_dup3;
  if (!no_dup3) {
    do
      r = uv__dup3(oldfd, newfd, UV__O_CLOEXEC);
    while (r == -1 && errno == EBUSY);
    if (r != -1)
      return r;
    if (errno != ENOSYS)
      return UV__ERR(errno);
    /* Fall through. */
    no_dup3 = 1;
  }
#endif
  {
    int err;
    do
      r = dup2(oldfd, newfd);
#if defined(__linux__)
    while (r == -1 && errno == EBUSY);
#else
    while (0);  /* Never retry. */
#endif

    if (r == -1)
      return UV__ERR(errno);

    err = uv__cloexec(newfd, 1);
    if (err) {
      uv__close(newfd);
      return err;
    }

    return r;
  }
}


int uv_os_homedir(char* buffer, size_t* size) {
  uv_passwd_t pwd;
  size_t len;
  int r;

  /* Check if the HOME environment variable is set first. The task of
     performing input validation on buffer and size is taken care of by
     uv_os_getenv(). */
  r = uv_os_getenv("HOME", buffer, size);

  if (r != UV_ENOENT)
    return r;

  /* HOME is not set, so call uv__getpwuid_r() */
  r = uv__getpwuid_r(&pwd);

  if (r != 0) {
    return r;
  }

  len = strlen(pwd.homedir);

  if (len >= *size) {
    *size = len + 1;
    uv_os_free_passwd(&pwd);
    return UV_ENOBUFS;
  }

  memcpy(buffer, pwd.homedir, len + 1);
  *size = len;
  uv_os_free_passwd(&pwd);

  return 0;
}


int uv_os_tmpdir(char* buffer, size_t* size) {
  const char* buf;
  size_t len;

  if (buffer == NULL || size == NULL || *size == 0)
    return UV_EINVAL;

#define CHECK_ENV_VAR(name)                                                   \
  do {                                                                        \
    buf = getenv(name);                                                       \
    if (buf != NULL)                                                          \
      goto return_buffer;                                                     \
  }                                                                           \
  while (0)

  /* Check the TMPDIR, TMP, TEMP, and TEMPDIR environment variables in order */
  CHECK_ENV_VAR("TMPDIR");
  CHECK_ENV_VAR("TMP");
  CHECK_ENV_VAR("TEMP");
  CHECK_ENV_VAR("TEMPDIR");

#undef CHECK_ENV_VAR

  /* No temp environment variables defined */
  #if defined(__ANDROID__)
    buf = "/data/local/tmp";
  #else
    buf = "/tmp";
  #endif

return_buffer:
  len = strlen(buf);

  if (len >= *size) {
    *size = len + 1;
    return UV_ENOBUFS;
  }

  /* The returned directory should not have a trailing slash. */
  if (len > 1 && buf[len - 1] == '/') {
    len--;
  }

  memcpy(buffer, buf, len + 1);
  buffer[len] = '\0';
  *size = len;

  return 0;
}


int uv__getpwuid_r(uv_passwd_t* pwd) {
  struct passwd pw;
  struct passwd* result;
  char* buf;
  uid_t uid;
  size_t bufsize;
  size_t name_size;
  size_t homedir_size;
  size_t shell_size;
  size_t gecos_size;
  long initsize;
  int r;
#if defined(__ANDROID_API__) && __ANDROID_API__ < 21
  int (*getpwuid_r)(uid_t, struct passwd*, char*, size_t, struct passwd**);

  getpwuid_r = dlsym(RTLD_DEFAULT, "getpwuid_r");
  if (getpwuid_r == NULL)
    return UV_ENOSYS;
#endif

  if (pwd == NULL)
    return UV_EINVAL;

  initsize = sysconf(_SC_GETPW_R_SIZE_MAX);

  if (initsize <= 0)
    bufsize = 4096;
  else
    bufsize = (size_t) initsize;

  uid = geteuid();
  buf = NULL;

  for (;;) {
    uv__free(buf);
    buf = uv__malloc(bufsize);

    if (buf == NULL)
      return UV_ENOMEM;

    r = getpwuid_r(uid, &pw, buf, bufsize, &result);

    if (r != ERANGE)
      break;

    bufsize *= 2;
  }

  if (r != 0) {
    uv__free(buf);
    return -r;
  }

  if (result == NULL) {
    uv__free(buf);
    return UV_ENOENT;
  }

  /* Allocate memory for the username, gecos, shell, and home directory. */
  name_size = strlen(pw.pw_name) + 1;
  homedir_size = strlen(pw.pw_dir) + 1;
  shell_size = strlen(pw.pw_shell) + 1;

#ifdef __MVS__
  gecos_size = 0; /* pw_gecos does not exist on zOS. */
#else
  if (pw.pw_gecos != NULL)
    gecos_size = strlen(pw.pw_gecos) + 1;
  else
    gecos_size = 0;
#endif

  pwd->username = uv__malloc(name_size +
                             homedir_size +
                             shell_size +
                             gecos_size);

  if (pwd->username == NULL) {
    uv__free(buf);
    return UV_ENOMEM;
  }

  /* Copy the username */
  memcpy(pwd->username, pw.pw_name, name_size);

  /* Copy the home directory */
  pwd->homedir = pwd->username + name_size;
  memcpy(pwd->homedir, pw.pw_dir, homedir_size);

  /* Copy the shell */
  pwd->shell = pwd->homedir + homedir_size;
  memcpy(pwd->shell, pw.pw_shell, shell_size);

  /* Copy the gecos field */
#ifdef __MVS__
  pwd->gecos = NULL;  /* pw_gecos does not exist on zOS. */
#else
  if (pw.pw_gecos == NULL) {
    pwd->gecos = NULL;
  } else {
    pwd->gecos = pwd->shell + shell_size;
    memcpy(pwd->gecos, pw.pw_gecos, gecos_size);
  }
#endif

  /* Copy the uid and gid */
  pwd->uid = pw.pw_uid;
  pwd->gid = pw.pw_gid;

  uv__free(buf);

  return 0;
}


void uv_os_free_passwd(uv_passwd_t* pwd) {
  if (pwd == NULL)
    return;

  /*
    The memory for name, shell, homedir, and gecos are allocated in a single
    uv__malloc() call. The base of the pointer is stored in pwd->username, so
    that is the field that needs to be freed.
  */
  uv__free(pwd->username);
  pwd->username = NULL;
  pwd->shell = NULL;
  pwd->homedir = NULL;
  pwd->gecos = NULL;
}


int uv_os_get_passwd(uv_passwd_t* pwd) {
  return uv__getpwuid_r(pwd);
}


int uv_translate_sys_error(int sys_errno) {
  /* If < 0 then it's already a libuv error. */
  return sys_errno <= 0 ? sys_errno : -sys_errno;
}


int uv_os_getenv(const char* name, char* buffer, size_t* size) {
  char* var;
  size_t len;

  if (name == NULL || buffer == NULL || size == NULL || *size == 0)
    return UV_EINVAL;

  var = getenv(name);

  if (var == NULL)
    return UV_ENOENT;

  len = strlen(var);

  if (len >= *size) {
    *size = len + 1;
    return UV_ENOBUFS;
  }

  memcpy(buffer, var, len + 1);
  *size = len;

  return 0;
}


int uv_os_setenv(const char* name, const char* value) {
  if (name == NULL || value == NULL)
    return UV_EINVAL;

  if (setenv(name, value, 1) != 0)
    return UV__ERR(errno);

  return 0;
}


int uv_os_unsetenv(const char* name) {
  if (name == NULL)
    return UV_EINVAL;

  if (unsetenv(name) != 0)
    return UV__ERR(errno);

  return 0;
}


int uv_os_gethostname(char* buffer, size_t* size) {
  /*
    On some platforms, if the input buffer is not large enough, gethostname()
    succeeds, but truncates the result. libuv can detect this and return ENOBUFS
    instead by creating a large enough buffer and comparing the hostname length
    to the size input.
  */
  char buf[MAXHOSTNAMELEN + 1];
  size_t len;

  if (buffer == NULL || size == NULL || *size == 0)
    return UV_EINVAL;

  if (gethostname(buf, sizeof(buf)) != 0)
    return UV__ERR(errno);

  buf[sizeof(buf) - 1] = '\0'; /* Null terminate, just to be safe. */
  len = strlen(buf);

  if (len >= *size) {
    *size = len + 1;
    return UV_ENOBUFS;
  }

  memcpy(buffer, buf, len + 1);
  *size = len;
  return 0;
}


uv_pid_t uv_os_getpid(void) {
  return getpid();
}


uv_pid_t uv_os_getppid(void) {
  return getppid();
}

int uv_cpumask_size(void) {
#if defined(__linux__) || defined(__FreeBSD__)
  return CPU_SETSIZE;
#else
  return UV_ENOTSUP;
#endif
}

int uv_os_getpriority(uv_pid_t pid, int* priority) {
  int r;

  if (priority == NULL)
    return UV_EINVAL;

  errno = 0;
  r = getpriority(PRIO_PROCESS, (int) pid);

  if (r == -1 && errno != 0)
    return UV__ERR(errno);

  *priority = r;
  return 0;
}


int uv_os_setpriority(uv_pid_t pid, int priority) {
  if (priority < UV_PRIORITY_HIGHEST || priority > UV_PRIORITY_LOW)
    return UV_EINVAL;

  if (setpriority(PRIO_PROCESS, (int) pid, priority) != 0)
    return UV__ERR(errno);

  return 0;
}


int uv__getsockpeername(const uv_handle_t* handle,
                        uv__peersockfunc func,
                        struct sockaddr* name,
                        int* namelen) {
  socklen_t socklen;
  uv_os_fd_t fd;
  int r;

  r = uv_fileno(handle, &fd);
  if (r < 0)
    return r;

  /* sizeof(socklen_t) != sizeof(int) on some systems. */
  socklen = (socklen_t) *namelen;

  if (func(fd, name, &socklen))
    return UV__ERR(errno);

  *namelen = (int) socklen;
  return 0;
}
