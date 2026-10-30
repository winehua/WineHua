#!/usr/bin/env python3
"""Test the actual resource reply dispatcher with real SCM_RIGHTS sockets."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def dispatcher(text):
    start = text.index('render_context_dispatch_create_resource(')
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return 'static bool\n' + text[start:end]


HARNESS = r'''
#define _GNU_SOURCE
#define __OHOS__ 1
#define ENABLE_SAME_PROCESS_RENDER_SERVER 1
#define ENABLE_RENDER_SERVER_WORKER_THREAD 1
#define UNUSED __attribute__((unused))
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#define VIRGL_RESOURCE_FD_INVALID -1
struct render_socket { int fd; };
struct render_context { uint32_t ctx_id; struct render_socket socket; };
struct render_context_op_create_resource_request {
 uint32_t res_id, blob_flags; uint64_t blob_id, blob_size;
};
union render_context_op_request { struct render_context_op_create_resource_request create_resource; };
struct render_context_op_create_resource_reply { int fd_type; uint32_t map_info; int vulkan_info; };
static int sender_fd;
static bool fail_create;
static bool render_state_create_resource(uint32_t c,uint32_t r,uint64_t b,uint64_t size,
 uint32_t flags,int *type,int *fd,uint32_t *map,int *info) {
 (void)c;(void)r;(void)b;(void)flags;(void)map;(void)info;
 if (fail_create) return false;
 sender_fd = *fd = memfd_create("resource-fd-regression", MFD_CLOEXEC);
 assert(*fd >= 0); assert(!ftruncate(*fd, size));
 assert(pwrite(*fd,"ok",2,0)==2); *type=2; return true;
}
static bool render_socket_send_reply(struct render_socket *s,const void *data,size_t n) {
 return send(s->fd,data,n,MSG_NOSIGNAL)==(ssize_t)n;
}
static bool render_socket_send_reply_with_fds(struct render_socket *s,const void *data,size_t n,
 const int *fds,int count) {
 assert(count==1);char control[CMSG_SPACE(sizeof(int))]={0};
 struct iovec iov={(void*)data,n};struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,
 .msg_control=control,.msg_controllen=sizeof control};
 struct cmsghdr *c=CMSG_FIRSTHDR(&msg);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;
 c->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(c),fds,sizeof(int));
 return sendmsg(s->fd,&msg,MSG_NOSIGNAL)==(ssize_t)n;
}
static int fd_count(void) {
 DIR*d=opendir("/proc/self/fd");assert(d);int n=0;struct dirent*e;
 while((e=readdir(d)))if(e->d_name[0]!='.')n++;closedir(d);return n;
}
FUNCTION
int main(void) {
 int pair[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair));
 struct render_context ctx={.ctx_id=1,.socket={pair[0]}};
 union render_context_op_request req={.create_resource={.res_id=1,.blob_id=2,.blob_size=4096}};
 int baseline=fd_count();
 for(int i=0;i<2048;i++) {
  assert(render_context_dispatch_create_resource(&ctx,&req,NULL,0));
  /* Sender closes before recv; the queued SCM_RIGHTS still pins storage. */
  assert(fcntl(sender_fd,F_GETFD)==-1 && errno==EBADF);
  char control[CMSG_SPACE(sizeof(int))]={0};struct render_context_op_create_resource_reply reply;
  struct iovec iov={&reply,sizeof reply};struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,
   .msg_control=control,.msg_controllen=sizeof control};
  assert(recvmsg(pair[1],&msg,0)==sizeof reply);struct cmsghdr*c=CMSG_FIRSTHDR(&msg);
  assert(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS);int received;
  memcpy(&received,CMSG_DATA(c),sizeof received);char b[3]={0};
  assert(pread(received,b,2,0)==2&&!strcmp(b,"ok"));close(received);
  assert(fd_count()==baseline);
 }
 close(pair[1]);assert(!render_context_dispatch_create_resource(&ctx,&req,NULL,0));
 assert(fcntl(sender_fd,F_GETFD)==-1&&errno==EBADF);close(pair[0]);
 assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair));ctx.socket.fd=pair[0];fail_create=true;
 baseline=fd_count();assert(render_context_dispatch_create_resource(&ctx,&req,NULL,0));
 struct render_context_op_create_resource_reply reply;assert(recv(pair[1],&reply,sizeof reply,0)==sizeof reply);
 assert(reply.fd_type==VIRGL_RESOURCE_FD_INVALID&&fd_count()==baseline);
 close(pair[0]);close(pair[1]);puts("PASS: 2048 deferred receives, fixed FD count, failed send/create cleanup");
}
'''


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'requires Linux cc')
class ResourceFdTests(unittest.TestCase):
    def run_dispatcher(self, source):
        with tempfile.TemporaryDirectory(prefix='winehua-fd-test-') as directory:
            src = Path(directory) / 'test.c'
            exe = Path(directory) / 'test'
            src.write_text(HARNESS.replace('FUNCTION', dispatcher(source)))
            subprocess.run(['cc', '-std=c11', '-O1', '-g', str(src), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], capture_output=True, text=True)

    def test_resource_reply_lifetime(self):
        result = self.run_dispatcher((ROOT / 'thirdparty/virglrenderer/server/render_context.c').read_text())
        self.assertEqual(result.returncode, 0, result.stderr)
        print(result.stdout.strip())

    def test_original_sender_retention_fails(self):
        source = (ROOT / 'thirdparty/virglrenderer/server/render_context.c').read_text()
        source = source.replace('   close(res_fd);', '   if (!ok) close(res_fd);', 1)
        self.assertNotEqual(self.run_dispatcher(source).returncode, 0)


if __name__ == '__main__':
    unittest.main()
