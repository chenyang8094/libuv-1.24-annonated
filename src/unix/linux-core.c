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

/* We lean on the fact that POLL{IN,OUT,ERR,HUP} correspond with their
 * EPOLL* counterparts.  We use the POLL* variants in this file because that
 * is what libuv uses elsewhere.
 */

#include "uv.h"
#include "internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <net/if.h>
#include <sys/epoll.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#define HAVE_IFADDRS_H 1

#ifdef __UCLIBC__
# if __UCLIBC_MAJOR__ < 0 && __UCLIBC_MINOR__ < 9 && __UCLIBC_SUBLEVEL__ < 32
#  undef HAVE_IFADDRS_H
# endif
#endif

#ifdef HAVE_IFADDRS_H
# if defined(__ANDROID__)
#  include "uv/android-ifaddrs.h"
# else
#  include <ifaddrs.h>
# endif
# include <sys/socket.h>
# include <net/ethernet.h>
# include <netpacket/packet.h>
#endif /* HAVE_IFADDRS_H */

/* Available from 2.6.32 onwards. */
#ifndef CLOCK_MONOTONIC_COARSE
# define CLOCK_MONOTONIC_COARSE 6
#endif

/* This is rather annoying: CLOCK_BOOTTIME lives in <linux/time.h> but we can't
 * include that file because it conflicts with <time.h>. We'll just have to
 * define it ourselves.
 */
#ifndef CLOCK_BOOTTIME
# define CLOCK_BOOTTIME 7
#endif

static int read_models(unsigned int numcpus, uv_cpu_info_t* ci);
static int read_times(FILE* statfile_fp,
                      unsigned int numcpus,
                      uv_cpu_info_t* ci);
static void read_speeds(unsigned int numcpus, uv_cpu_info_t* ci);
static unsigned long read_cpufreq(unsigned int cpunum);


/*  loop平台相关的初始化 */
int uv__platform_loop_init(uv_loop_t* loop) {
  int fd;
  
  /* 
    这是比较新的内核提供的接口：https://www.systutorials.com/docs/linux/man/2-epoll_create1/ 
    同时注意这里传入的EPOLL_CLOEXEC标识，该标志的作用可以参见：https://www.systutorials.com/docs/linux/man/2-open/
    */
  fd = epoll_create1(EPOLL_CLOEXEC);

  /* epoll_create1() can fail either because it's not implemented (old kernel)
   * or because it doesn't understand the EPOLL_CLOEXEC flag.
   */
  if (fd == -1 && (errno == ENOSYS || errno == EINVAL)) {
    /* 创建后端fd 
    原型：int epoll_create(int size);
    细节参见： http://man7.org/linux/man-pages/man2/epoll_create.2.html
     */
    fd = epoll_create(256);

    /* 由于epoll_create不支持设置CLOEXEC标志，因此这里需要单独用光fctl设置
    设置FIOCLEX（set close on exec on fd ）标志 */
    if (fd != -1)
      uv__cloexec(fd, 1);
  }
  
  /* 设置后端fd  */
  loop->backend_fd = fd;
  /*   */
  loop->inotify_fd = -1;
  /*   */
  loop->inotify_watchers = NULL;

  if (fd == -1)
    return UV__ERR(errno);

  return 0;
}

/*    */
int uv__io_fork(uv_loop_t* loop) {
  int err;
  void* old_watchers;

  old_watchers = loop->inotify_watchers;

  uv__close(loop->backend_fd);
  loop->backend_fd = -1;
  uv__platform_loop_delete(loop);

  err = uv__platform_loop_init(loop);
  if (err)
    return err;

  return uv__inotify_fork(loop, old_watchers);
}

/*  删除loop  */
void uv__platform_loop_delete(uv_loop_t* loop) {
  /*    */
  if (loop->inotify_fd == -1) return;
  /*    */
  uv__io_stop(loop, &loop->inotify_read_watcher, POLLIN);
  uv__close(loop->inotify_fd);
  loop->inotify_fd = -1;
}


void uv__platform_invalidate_fd(uv_loop_t* loop, int fd) {
  struct epoll_event* events;
  struct epoll_event dummy;
  uintptr_t i;
  uintptr_t nfds;

  assert(loop->watchers != NULL);

  /* loop->watchers最后两项的特殊用途 */
  events = (struct epoll_event*) loop->watchers[loop->nwatchers];
  nfds = (uintptr_t) loop->watchers[loop->nwatchers + 1];
  if (events != NULL)
    /* Invalidate events with same file descriptor */
    for (i = 0; i < nfds; i++)
      if (events[i].data.fd == fd)
        events[i].data.fd = -1;

  /* Remove the file descriptor from the epoll.
   * This avoids a problem where the same file description remains open
   * in another process, causing repeated junk epoll events.
   *
   * We pass in a dummy epoll_event, to work around a bug in old kernels.
   */
  if (loop->backend_fd >= 0) {
    /* Work around a bug in kernels 3.10 to 3.19 where passing a struct that
     * has the EPOLLWAKEUP flag set generates spurious audit syslog warnings.
     */
    memset(&dummy, 0, sizeof(dummy));
    epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, &dummy);
  }
}


int uv__io_check_fd(uv_loop_t* loop, int fd) {
  struct epoll_event e;
  int rc;

  e.events = POLLIN;
  e.data.fd = -1;

  rc = 0;
  if (epoll_ctl(loop->backend_fd, EPOLL_CTL_ADD, fd, &e))
    if (errno != EEXIST)
      rc = UV__ERR(errno);

  if (rc == 0)
    if (epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, &e))
      abort();

  return rc;
}

/* 处理io事件轮询 */
void uv__io_poll(uv_loop_t* loop, int timeout) {
  /* A bug in kernels < 2.6.37 makes timeouts larger than ~30 minutes
   * effectively infinite on 32 bits architectures.  To avoid blocking
   * indefinitely, we cap the timeout and poll again if necessary.
   *
   * Note that "30 minutes" is a simplification because it depends on
   * the value of CONFIG_HZ.  The magic constant assumes CONFIG_HZ=1200,
   * that being the largest value I have seen in the wild (and only once.)
   */
  static const int max_safe_timeout = 1789569;
  struct epoll_event events[1024];
  struct epoll_event* pe;
  struct epoll_event e;
  int real_timeout;
  QUEUE* q;
  uv__io_t* w;
  sigset_t sigset;
  sigset_t* psigset;
  uint64_t base;
  int have_signals;
  int nevents;
  int count;
  int nfds;
  int fd;
  int op;
  int i;

  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }
  /* 如果该loop上的watcher队列不为空，则遍历队列 */
  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    /* 取出watcher队列头节点 */
    q = QUEUE_HEAD(&loop->watcher_queue);
    /* 从watcher队列中删除该节点 */
    QUEUE_REMOVE(q);
    /* 将取出的结点前后向指针指向自己，完全脱离队列 */
    QUEUE_INIT(q);

    /* 根据取出的队列节点还原watcher结构，典型的contaner_of用法 */
    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    /* 确保该watcher有待处理的悬挂事件pending events，可以为EPOLLIN、EPOLLOUT、EPOLLRDHUP、EPOLLPRI、
      EPOLLERR、EPOLLET、EPOLLONESHOT、EPOLLWAKEUP、EPOLLEXCLUSIVE */
    assert(w->pevents != 0);
    /* 该watcher绑定的fd必须是有效的 */
    assert(w->fd >= 0);
    /* 这个在maybe_resize函数中保证 */
    assert(w->fd < (int) loop->nwatchers);
   
    /* 初始化epoll要监听的事件为watcher的Pending event */
    e.events = w->pevents;
    /* 初始化epoll要监听的fd为watcher绑定的fd */
    e.data.fd = w->fd;

    /* 如果该watcher当前已经注册的events为0，即之前没有注册过event，则此次就是EPOLL_CTL_ADD，否则为EPOLL_CTL_MOD */
    if (w->events == 0)
      op = EPOLL_CTL_ADD;
    else
      op = EPOLL_CTL_MOD;

    /* XXX Future optimization: do EPOLL_CTL_MOD lazily if we stop watching
     * events, skip the syscall and squelch the events after epoll_wait().
     * 
     * 向epoll注册事件
     * 原型：int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
     * 细节参见：http://man7.org/linux/man-pages/man2/epoll_ctl.2.html 
     */
    if (epoll_ctl(loop->backend_fd, op, w->fd, &e)) {
      /* 如果发生错误，并且错误不是EEXIST，则发生异常错误，直接abort */
      if (errno != EEXIST)
        abort();

      /* 否则，就是EEXIST错误，表示对一个已经注册的fd执行EPOLL_CTL_ADD操作（一个fd允许被绑定到多个watcher） */
      assert(op == EPOLL_CTL_ADD);

      /* 重新用EPOLL_CTL_MOD来注册这个事件 */
      if (epoll_ctl(loop->backend_fd, EPOLL_CTL_MOD, w->fd, &e)){
            /* 如果还出错就直接abort */
            abort();
      } 
    }
    /* 把该watcher的当前events更新为pevents */
    w->events = w->pevents;
  }

  /* 至此watcher队列遍历完毕，所有的watcher对应的事件都已经被注册到epoll上 */

  /* psigset初始化 */
  psigset = NULL;
  /* 如果loop需要在epoll_pwait时候临时屏蔽SIGPROF信号 */
  if (loop->flags & UV_LOOP_BLOCK_SIGPROF) {
    /* 设置SIGPROF */
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGPROF);
    psigset = &sigset;
  }
  
  /* -1：阻塞  0：非阻塞  >0: 最长阻塞timeout时间 */
  assert(timeout >= -1);
  /* 保存当前时间，即本次poll的开始时间 */
  base = loop->time;
  count = 48; /* Benchmarks suggest this gives the best throughput. */
  /* 保存超时时间（长度） */
  real_timeout = timeout;

  for (;;) {
    /* See the comment for max_safe_timeout for an explanation of why
     * this is necessary.  Executive summary: kernel bug workaround.
     */
    if (sizeof(int32_t) == sizeof(long) && timeout >= max_safe_timeout)
      timeout = max_safe_timeout;
    
    /* 在epoll fd上查询所有注册的事件,每次最多返回1024个事件，该系统调用最长会被阻塞timeout 
    原型：int epoll_pwait(int epfd, struct epoll_event *events,int maxevents, int timeout, 
          const sigset_t *sigmask);
    细节参见: http://man7.org/linux/man-pages/man2/epoll_wait.2.html */
    nfds = epoll_pwait(loop->backend_fd,
                       events,
                       ARRAY_SIZE(events),
                       timeout,
                       psigset);

    /* Update loop->time unconditionally. It's tempting to skip the update when
     * timeout == 0 (i.e. non-blocking poll) but there is no guarantee that the
     * operating system didn't reschedule our process while in the syscall.
     * 
     * 无条件更新loop->time = uv__hrtime(UV_CLOCK_FAST) / 1000000;
     * SAVE_ERRNO这一句相当于一下代码：
     *   int _saved_errno = errno;                                                 
     *   uv__update_time(loop)                                            
     *   errno = _saved_errno;   
     * 
     * 这么做的原因是不能因为uv__update_time出错而导致errno被改变，因为后文会用到errno来判断epoll_pwait
     * 状态  
     */
    SAVE_ERRNO(uv__update_time(loop));

    /* 0表示没有就绪的fd */
    if (nfds == 0) {
      /* 不能是阻塞模式 */
      assert(timeout != -1);

      /* 非阻塞模式则直接返回 */
      if (timeout == 0)
        return;

      /* We may have been inside the system call for longer than |timeout|
       * milliseconds so we need to update the timestamp to avoid drift.
       * 否则就一定是超时，跳转到update_timeout更新超时时间
       */
      goto update_timeout;
    }

    /* 发生错误 */
    if (nfds == -1) {
      /* 如果不是被信号中断发生的错误则abort */
      if (errno != EINTR)
        abort();

      /* 如果是在阻塞模式下被信号中断，则继续下一次epoll_pwait */
      if (timeout == -1)
        continue;

      /* 如果是在非阻塞模式下被信号中断，则返回 */
      if (timeout == 0)
        return;

      /* 如果是有超时时间情况下被信号打断，则更新timeout 进行下一次 poll */
      goto update_timeout;
    }

    /* 能运行到这里，说明一定有就绪的fd事件 */

    /* 是否有信号 */
    have_signals = 0;
    /* 已处理的事件个数 */
    nevents = 0;
    
    /* watchers不为空指针（uv__io_t** watchers）  */
    assert(loop->watchers != NULL);
    /* loop->watchers数组向的最后两项分别存放了本次轮询后就绪的事件和就绪的fd个数,
    注意直接把void *赋值给特定类型在c语言中是正确的，但是在c++中是错误的 */
    loop->watchers[loop->nwatchers] = (void*) events;
    loop->watchers[loop->nwatchers + 1] = (void*) (uintptr_t) nfds;
    /* 遍历所有就绪的fd */
    for (i = 0; i < nfds; i++) {
      /* 第i个就绪的事件 */
      pe = events + i; 
      /* 第i个就绪的事件对应的fd */
      fd = pe->data.fd;

      /* Skip invalidated events, see uv__platform_invalidate_fd */
      if (fd == -1)
        continue;

      assert(fd >= 0);
      /*  */
      assert((unsigned) fd < loop->nwatchers);
      
      /* 根据fd取出对应的watcher */
      w = loop->watchers[fd];
      /* 如果该watcher为空指针 */
      if (w == NULL) {
        /* File descriptor that we've stopped watching, disarm it.
         *
         * Ignore all errors because we may be racing with another thread
         * when the file descriptor is closed.
         * 
         * 说明这个fd已经被关闭了，忽略epoll_ctl返回的任何错误
         */
        epoll_ctl(loop->backend_fd, EPOLL_CTL_DEL, fd, pe);
        continue;
      }

      /* Give users only events they're interested in. Prevents spurious
       * callbacks when previous callback invocation in this loop has stopped
       * the current watcher. Also, filters out events that users has not
       * requested us to watch.
       * 
       * 仅为用户提供他们感兴趣的事件。当此循环中的先前回调调用已停止当前观察者时，
       * 防止虚假回调。此外，过滤掉用户未请求我们观看的事件。这里注意EPOLLERR和POLLHUP会在
       * epoll_pwait中被无条件加入到events中，因此这里也需要保留。
       */
      pe->events &= w->pevents | POLLERR | POLLHUP;

      /* Work around an epoll quirk where it sometimes reports just the
       * EPOLLERR or EPOLLHUP event.  In order to force the event loop to
       * move forward, we merge in the read/write events that the watcher
       * is interested in; uv__read() and uv__write() will then deal with
       * the error or hangup in the usual fashion.
       * 
       * 为了解决一个epoll怪癖，它有时只报告EPOLLERR或EPOLLHUP事件。为了强制事件循环向前移动，
       * 我们合并了watcher感兴趣的读/写事件; 然后uv__read（）和uv__write（）将以通常的方式处理错误或挂起。
       *
       * Note to self: happens when epoll reports EPOLLIN|EPOLLHUP, the user
       * reads the available data, calls uv_read_stop(), then sometime later
       * calls uv_read_start() again.  By then, libuv has forgotten about the
       * hangup and the kernel won't report EPOLLIN again because there's
       * nothing left to read.  If anything, libuv is to blame here.  The
       * current hack is just a quick bandaid; to properly fix it, libuv
       * needs to remember the error/hangup event.  We should get that for
       * free when we switch over to edge-triggered I/O.
       * 
       * 
       */
      if (pe->events == POLLERR || pe->events == POLLHUP)
        pe->events |=
          w->pevents & (POLLIN | POLLOUT | UV__POLLRDHUP | UV__POLLPRI);
      
      /* 如果事件不为空 */
      if (pe->events != 0) {
        /* Run signal watchers last.  This also affects child process watchers
         * because those are implemented in terms of signal watchers.
         * 
         */
        if (w == &loop->signal_io_watcher)
          have_signals = 1;/* 如果该watcher是loop->signal_io_watcher，
                             标记有信号要处理，暂时不处理这个信号，等到所有fd处理完循环退出后再处理 */
        else /* 不是信号的事件直接进行回调处理 */
          w->cb(loop, w, pe->events);
        
        /* 已处理的事件计数 */
        nevents++;
      }
    }

    /* 有信号要处理,就调用signal_io_watcher回调 */
    if (have_signals != 0)
      loop->signal_io_watcher.cb(loop, &loop->signal_io_watcher, POLLIN);

    /* 清空，准备下一次轮询 */ 
    loop->watchers[loop->nwatchers] = NULL;
    loop->watchers[loop->nwatchers + 1] = NULL;

    /* 有信号为什么就需要退出？？？ */
    if (have_signals != 0)
      return;  /* Event loop should cycle now so don't poll again. */

    /* 已处理的事件不为0 */ 
    if (nevents != 0) {
      /* 已处理的事件不为0，且本次epoll_pwait返回值nfds正好等于ARRAY_SIZE(events)，说明可能有更多
      的就绪事件等待被处理，因此还需要再次轮询。但是为了避免一次性处理太多事件而导致整个事件循环被阻塞过长
      时间，这里用count来控制 */ 
      if (nfds == ARRAY_SIZE(events) && --count != 0) {
        /* 继续下一轮epoll_pwait，但是此次timeout为0，不阻塞，因此轮询会很快 */
        timeout = 0;
        continue;
      }

      /* 如果nfds不等于（其实就是小于）ARRAY_SIZE(events)，那就说明此次轮询就绪的事件就这么多，没没要再
      轮询了，直接返回上层loop大循环 */ 
      return;
    }

    /* 运行都这里，说明nevents为0，即没有处理信号也没有处理事件 */
     
    /* 非阻塞模式，则退出函数 */
    if (timeout == 0)
      return;

    /* 阻塞模式，则继续下一次epoll_pwait */
    if (timeout == -1)
      continue;

    /* 更新超时时间 */
update_timeout:
    assert(timeout > 0);

    /* loop->time - base为已经过去的时间，real_timeout减去已经过去的时间就是剩下的timeout */
    real_timeout -= (loop->time - base);
    /* real_timeout <= 0表示已经超时，则直接返回、退出循环 */
    if (real_timeout <= 0)
      return;

    /* 更新timeout，进入下一次epoll_pwait轮询 */
    timeout = real_timeout;
  }
}


uint64_t uv__hrtime(uv_clocktype_t type) {
  static clock_t fast_clock_id = -1;
  struct timespec t;
  clock_t clock_id;

  /* Prefer CLOCK_MONOTONIC_COARSE if available but only when it has
   * millisecond granularity or better.  CLOCK_MONOTONIC_COARSE is
   * serviced entirely from the vDSO, whereas CLOCK_MONOTONIC may
   * decide to make a costly system call.
   */
  /* TODO(bnoordhuis) Use CLOCK_MONOTONIC_COARSE for UV_CLOCK_PRECISE
   * when it has microsecond granularity or better (unlikely).
   */
  if (type == UV_CLOCK_FAST && fast_clock_id == -1) {
    if (clock_getres(CLOCK_MONOTONIC_COARSE, &t) == 0 &&
        t.tv_nsec <= 1 * 1000 * 1000) {
      fast_clock_id = CLOCK_MONOTONIC_COARSE;
    } else {
      fast_clock_id = CLOCK_MONOTONIC;
    }
  }

  clock_id = CLOCK_MONOTONIC;
  if (type == UV_CLOCK_FAST)
    clock_id = fast_clock_id;

  if (clock_gettime(clock_id, &t))
    return 0;  /* Not really possible. */

  return t.tv_sec * (uint64_t) 1e9 + t.tv_nsec;
}


int uv_resident_set_memory(size_t* rss) {
  char buf[1024];
  const char* s;
  ssize_t n;
  long val;
  int fd;
  int i;

  do
    fd = open("/proc/self/stat", O_RDONLY);
  while (fd == -1 && errno == EINTR);

  if (fd == -1)
    return UV__ERR(errno);

  do
    n = read(fd, buf, sizeof(buf) - 1);
  while (n == -1 && errno == EINTR);

  uv__close(fd);
  if (n == -1)
    return UV__ERR(errno);
  buf[n] = '\0';

  s = strchr(buf, ' ');
  if (s == NULL)
    goto err;

  s += 1;
  if (*s != '(')
    goto err;

  s = strchr(s, ')');
  if (s == NULL)
    goto err;

  for (i = 1; i <= 22; i++) {
    s = strchr(s + 1, ' ');
    if (s == NULL)
      goto err;
  }

  errno = 0;
  val = strtol(s, NULL, 10);
  if (errno != 0)
    goto err;
  if (val < 0)
    goto err;

  *rss = val * getpagesize();
  return 0;

err:
  return UV_EINVAL;
}


int uv_uptime(double* uptime) {
  static volatile int no_clock_boottime;
  struct timespec now;
  int r;

  /* Try CLOCK_BOOTTIME first, fall back to CLOCK_MONOTONIC if not available
   * (pre-2.6.39 kernels). CLOCK_MONOTONIC doesn't increase when the system
   * is suspended.
   */
  if (no_clock_boottime) {
    retry: r = clock_gettime(CLOCK_MONOTONIC, &now);
  }
  else if ((r = clock_gettime(CLOCK_BOOTTIME, &now)) && errno == EINVAL) {
    no_clock_boottime = 1;
    goto retry;
  }

  if (r)
    return UV__ERR(errno);

  *uptime = now.tv_sec;
  return 0;
}


static int uv__cpu_num(FILE* statfile_fp, unsigned int* numcpus) {
  unsigned int num;
  char buf[1024];

  if (!fgets(buf, sizeof(buf), statfile_fp))
    return UV_EIO;

  num = 0;
  while (fgets(buf, sizeof(buf), statfile_fp)) {
    if (strncmp(buf, "cpu", 3))
      break;
    num++;
  }

  if (num == 0)
    return UV_EIO;

  *numcpus = num;
  return 0;
}


int uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
  unsigned int numcpus;
  uv_cpu_info_t* ci;
  int err;
  FILE* statfile_fp;

  *cpu_infos = NULL;
  *count = 0;

  statfile_fp = uv__open_file("/proc/stat");
  if (statfile_fp == NULL)
    return UV__ERR(errno);

  err = uv__cpu_num(statfile_fp, &numcpus);
  if (err < 0)
    goto out;

  err = UV_ENOMEM;
  ci = uv__calloc(numcpus, sizeof(*ci));
  if (ci == NULL)
    goto out;

  err = read_models(numcpus, ci);
  if (err == 0)
    err = read_times(statfile_fp, numcpus, ci);

  if (err) {
    uv_free_cpu_info(ci, numcpus);
    goto out;
  }

  /* read_models() on x86 also reads the CPU speed from /proc/cpuinfo.
   * We don't check for errors here. Worst case, the field is left zero.
   */
  if (ci[0].speed == 0)
    read_speeds(numcpus, ci);

  *cpu_infos = ci;
  *count = numcpus;
  err = 0;

out:

  if (fclose(statfile_fp))
    if (errno != EINTR && errno != EINPROGRESS)
      abort();

  return err;
}


static void read_speeds(unsigned int numcpus, uv_cpu_info_t* ci) {
  unsigned int num;

  for (num = 0; num < numcpus; num++)
    ci[num].speed = read_cpufreq(num) / 1000;
}


/* Also reads the CPU frequency on x86. The other architectures only have
 * a BogoMIPS field, which may not be very accurate.
 *
 * Note: Simply returns on error, uv_cpu_info() takes care of the cleanup.
 */
static int read_models(unsigned int numcpus, uv_cpu_info_t* ci) {
  static const char model_marker[] = "model name\t: ";
  static const char speed_marker[] = "cpu MHz\t\t: ";
  const char* inferred_model;
  unsigned int model_idx;
  unsigned int speed_idx;
  char buf[1024];
  char* model;
  FILE* fp;

  /* Most are unused on non-ARM, non-MIPS and non-x86 architectures. */
  (void) &model_marker;
  (void) &speed_marker;
  (void) &speed_idx;
  (void) &model;
  (void) &buf;
  (void) &fp;

  model_idx = 0;
  speed_idx = 0;

#if defined(__arm__) || \
    defined(__i386__) || \
    defined(__mips__) || \
    defined(__x86_64__)
  fp = uv__open_file("/proc/cpuinfo");
  if (fp == NULL)
    return UV__ERR(errno);

  while (fgets(buf, sizeof(buf), fp)) {
    if (model_idx < numcpus) {
      if (strncmp(buf, model_marker, sizeof(model_marker) - 1) == 0) {
        model = buf + sizeof(model_marker) - 1;
        model = uv__strndup(model, strlen(model) - 1);  /* Strip newline. */
        if (model == NULL) {
          fclose(fp);
          return UV_ENOMEM;
        }
        ci[model_idx++].model = model;
        continue;
      }
    }
#if defined(__arm__) || defined(__mips__)
    if (model_idx < numcpus) {
#if defined(__arm__)
      /* Fallback for pre-3.8 kernels. */
      static const char model_marker[] = "Processor\t: ";
#else	/* defined(__mips__) */
      static const char model_marker[] = "cpu model\t\t: ";
#endif
      if (strncmp(buf, model_marker, sizeof(model_marker) - 1) == 0) {
        model = buf + sizeof(model_marker) - 1;
        model = uv__strndup(model, strlen(model) - 1);  /* Strip newline. */
        if (model == NULL) {
          fclose(fp);
          return UV_ENOMEM;
        }
        ci[model_idx++].model = model;
        continue;
      }
    }
#else  /* !__arm__ && !__mips__ */
    if (speed_idx < numcpus) {
      if (strncmp(buf, speed_marker, sizeof(speed_marker) - 1) == 0) {
        ci[speed_idx++].speed = atoi(buf + sizeof(speed_marker) - 1);
        continue;
      }
    }
#endif  /* __arm__ || __mips__ */
  }

  fclose(fp);
#endif  /* __arm__ || __i386__ || __mips__ || __x86_64__ */

  /* Now we want to make sure that all the models contain *something* because
   * it's not safe to leave them as null. Copy the last entry unless there
   * isn't one, in that case we simply put "unknown" into everything.
   */
  inferred_model = "unknown";
  if (model_idx > 0)
    inferred_model = ci[model_idx - 1].model;

  while (model_idx < numcpus) {
    model = uv__strndup(inferred_model, strlen(inferred_model));
    if (model == NULL)
      return UV_ENOMEM;
    ci[model_idx++].model = model;
  }

  return 0;
}


static int read_times(FILE* statfile_fp,
                      unsigned int numcpus,
                      uv_cpu_info_t* ci) {
  unsigned long clock_ticks;
  struct uv_cpu_times_s ts;
  unsigned long user;
  unsigned long nice;
  unsigned long sys;
  unsigned long idle;
  unsigned long dummy;
  unsigned long irq;
  unsigned int num;
  unsigned int len;
  char buf[1024];

  clock_ticks = sysconf(_SC_CLK_TCK);
  assert(clock_ticks != (unsigned long) -1);
  assert(clock_ticks != 0);

  rewind(statfile_fp);

  if (!fgets(buf, sizeof(buf), statfile_fp))
    abort();

  num = 0;

  while (fgets(buf, sizeof(buf), statfile_fp)) {
    if (num >= numcpus)
      break;

    if (strncmp(buf, "cpu", 3))
      break;

    /* skip "cpu<num> " marker */
    {
      unsigned int n;
      int r = sscanf(buf, "cpu%u ", &n);
      assert(r == 1);
      (void) r;  /* silence build warning */
      for (len = sizeof("cpu0"); n /= 10; len++);
    }

    /* Line contains user, nice, system, idle, iowait, irq, softirq, steal,
     * guest, guest_nice but we're only interested in the first four + irq.
     *
     * Don't use %*s to skip fields or %ll to read straight into the uint64_t
     * fields, they're not allowed in C89 mode.
     */
    if (6 != sscanf(buf + len,
                    "%lu %lu %lu %lu %lu %lu",
                    &user,
                    &nice,
                    &sys,
                    &idle,
                    &dummy,
                    &irq))
      abort();

    ts.user = clock_ticks * user;
    ts.nice = clock_ticks * nice;
    ts.sys  = clock_ticks * sys;
    ts.idle = clock_ticks * idle;
    ts.irq  = clock_ticks * irq;
    ci[num++].cpu_times = ts;
  }
  assert(num == numcpus);

  return 0;
}


static unsigned long read_cpufreq(unsigned int cpunum) {
  unsigned long val;
  char buf[1024];
  FILE* fp;

  snprintf(buf,
           sizeof(buf),
           "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq",
           cpunum);

  fp = uv__open_file(buf);
  if (fp == NULL)
    return 0;

  if (fscanf(fp, "%lu", &val) != 1)
    val = 0;

  fclose(fp);

  return val;
}


void uv_free_cpu_info(uv_cpu_info_t* cpu_infos, int count) {
  int i;

  for (i = 0; i < count; i++) {
    uv__free(cpu_infos[i].model);
  }

  uv__free(cpu_infos);
}

static int uv__ifaddr_exclude(struct ifaddrs *ent, int exclude_type) {
  if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)))
    return 1;
  if (ent->ifa_addr == NULL)
    return 1;
  /*
   * On Linux getifaddrs returns information related to the raw underlying
   * devices. We're not interested in this information yet.
   */
  if (ent->ifa_addr->sa_family == PF_PACKET)
    return exclude_type;
  return !exclude_type;
}

int uv_interface_addresses(uv_interface_address_t** addresses, int* count) {
#ifndef HAVE_IFADDRS_H
  *count = 0;
  *addresses = NULL;
  return UV_ENOSYS;
#else
  struct ifaddrs *addrs, *ent;
  uv_interface_address_t* address;
  int i;
  struct sockaddr_ll *sll;

  *count = 0;
  *addresses = NULL;

  if (getifaddrs(&addrs))
    return UV__ERR(errno);

  /* Count the number of interfaces */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (uv__ifaddr_exclude(ent, UV__EXCLUDE_IFADDR))
      continue;

    (*count)++;
  }

  if (*count == 0) {
    freeifaddrs(addrs);
    return 0;
  }

  *addresses = uv__malloc(*count * sizeof(**addresses));
  if (!(*addresses)) {
    freeifaddrs(addrs);
    return UV_ENOMEM;
  }

  address = *addresses;

  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (uv__ifaddr_exclude(ent, UV__EXCLUDE_IFADDR))
      continue;

    address->name = uv__strdup(ent->ifa_name);

    if (ent->ifa_addr->sa_family == AF_INET6) {
      address->address.address6 = *((struct sockaddr_in6*) ent->ifa_addr);
    } else {
      address->address.address4 = *((struct sockaddr_in*) ent->ifa_addr);
    }

    if (ent->ifa_netmask->sa_family == AF_INET6) {
      address->netmask.netmask6 = *((struct sockaddr_in6*) ent->ifa_netmask);
    } else {
      address->netmask.netmask4 = *((struct sockaddr_in*) ent->ifa_netmask);
    }

    address->is_internal = !!(ent->ifa_flags & IFF_LOOPBACK);

    address++;
  }

  /* Fill in physical addresses for each interface */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (uv__ifaddr_exclude(ent, UV__EXCLUDE_IFPHYS))
      continue;

    address = *addresses;

    for (i = 0; i < (*count); i++) {
      if (strcmp(address->name, ent->ifa_name) == 0) {
        sll = (struct sockaddr_ll*)ent->ifa_addr;
        memcpy(address->phys_addr, sll->sll_addr, sizeof(address->phys_addr));
      } else {
        memset(address->phys_addr, 0, sizeof(address->phys_addr));
      }
      address++;
    }
  }

  freeifaddrs(addrs);

  return 0;
#endif
}


void uv_free_interface_addresses(uv_interface_address_t* addresses,
  int count) {
  int i;

  for (i = 0; i < count; i++) {
    uv__free(addresses[i].name);
  }

  uv__free(addresses);
}


void uv__set_process_title(const char* title) {
#if defined(PR_SET_NAME)
  prctl(PR_SET_NAME, title);  /* Only copies first 16 characters. */
#endif
}
