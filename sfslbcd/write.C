/*
 *
 * Copyright (C) 2002 Benjie Chen (benjie@lcs.mit.edu)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 */

#include "sfslbcd.h"
#include "lbfs_prot.h"

struct write_obj {
  static const int PARALLEL_WRITES = 8;
  typedef callback<void,fattr3,bool>::ref cb_t;

  cb_t cb;
  ref<server> srv;
  nfs_fh3 fh;
  fattr3 fa;
  AUTH *auth;
  int fd;
  uint64 size;
  uint64 written;
  unsigned int outstanding_writes;
  bool callback;
  bool commit;
  
  void
  commit_reply(time_t rqtime, ref<commit3args> arg,
               ref<ex_commit3res> res, clnt_stat err) {
    outstanding_writes--;
    if (!err) {
      srv->getxattr (rqtime, NFSPROC3_COMMIT, 0, arg, res);
      if (res->resok->file_wcc.before.present &&
	  res->resok->file_wcc.after.present) {
        if ((res->resok->file_wcc.before.attributes)->size == fa.size &&
	    (res->resok->file_wcc.before.attributes)->mtime == fa.mtime) {
	  ex_fattr3 *f = res->resok->file_wcc.after.attributes;
	  fa = *reinterpret_cast<fattr3 *> (f);
	}
	else {
	  warn << "wcc check failed: reply out of order, or conflict\n";
          warn << "commit wcc failed: "
	       << fa.size << ":" << fa.mtime.seconds << ":"
	       << fa.ctime.seconds << " -- "
	       << (res->resok->file_wcc.before.attributes)->size << ":"
	       << (res->resok->file_wcc.before.attributes)->mtime.seconds << ":"
	       << "\n";
	}
      }
    }
    if (!callback && !err && res->status == NFS3_OK) {
      ok();
      return;
    }
    fail();
  }

  void
  write_reply(time_t rqtime, ref<write3args> arg,
              ref<ex_write3res> res, clnt_stat err) {
    outstanding_writes--;
    if (!err) {
      srv->getxattr (rqtime, NFSPROC3_WRITE, 0, arg, res);
      if (res->resok->file_wcc.before.present &&
	  res->resok->file_wcc.after.present) {
        if ((res->resok->file_wcc.before.attributes)->size == fa.size &&
	    (res->resok->file_wcc.before.attributes)->mtime == fa.mtime) {
	  ex_fattr3 *f = res->resok->file_wcc.after.attributes;
	  fa = *reinterpret_cast<fattr3 *> (f);
	}
	else {
	  warn << "wcc check failed: reply out of order, or conflict\n";
          warn << "write wcc failed: "
	       << fa.size << ":" << fa.mtime.seconds << ":"
	       << fa.ctime.seconds << " -- "
	       << (res->resok->file_wcc.before.attributes)->size << ":"
	       << (res->resok->file_wcc.before.attributes)->mtime.seconds << ":"
	       << "\n";
	}
      }
    }
    if (!callback && !err && res->status == NFS3_OK) {
      do_write();
      ok();
      return;
    }
    fail();
  }

  int nfs3_write (uint64 off, uint32 size) 
  {
    if (lseek(fd, off, SEEK_SET) < 0) {
      perror("lseek in cache file");
      fail();
      return -1;
    }
    ref<write3args> a = New refcounted<write3args>;
    a->file = fh;
    a->offset = off;
    a->count = size;
    a->stable = UNSTABLE;
    a->data.setsize(size);
    unsigned t = 0;
    while (t < size) {
      int n = read(fd, a->data.base()+t, size-t);
      if (n <= 0) {
	perror("reading from cache file");
	fail();
	return -1;
      }
      t += n;
    }
    outstanding_writes++;
    ref<ex_write3res> res = New refcounted <ex_write3res>;
    srv->nfsc->call (lbfs_NFSPROC3_WRITE, a, res,
	             wrap (this, &write_obj::write_reply, timenow, a, res),
		     auth);
    return t;
  }

  bool do_write() {
    if (written < size) {
      unsigned s = size-written;
      s = s > srv->wtpref ? srv->wtpref : s;
      if (nfs3_write (written, s) < 0)
	return true;
      written += s;
    }
    if (written == size && !commit) {
      commit = true;
      ref<commit3args> a = New refcounted<commit3args>;
      a->file = fh;
      a->offset = 0;
      a->count = 0;
      outstanding_writes++;
      ref<ex_commit3res> res = New refcounted <ex_commit3res>;
      srv->nfsc->call (lbfs_NFSPROC3_COMMIT, a, res,
	               wrap (this, &write_obj::commit_reply,
			     timenow, a, res),
		       auth);
    }
    if (written < size)
      return false;
    return true;
  }

  void fail() {
    if (!callback) {
      close(fd);
      callback = true;
      cb(fa, false);
    }
    if (outstanding_writes == 0)
      delete this;
  }

  void ok() {
    if (outstanding_writes == 0) {
      if (!callback) {
	close(fd);
        callback = true;
	cb(fa, true);
      }
      delete this;
    }
  }

  write_obj (str fn, nfs_fh3 fh, uint64 size, fattr3 fa, ref<server> srv,
            AUTH *a, write_obj::cb_t cb)
    : cb(cb), srv(srv), fh(fh), fa(fa), auth(a), size(size), written(0),
      outstanding_writes(0), callback(false), commit(false)
  {
    fd = open (fn, O_RDONLY);
    if (fd < 0) {
      perror("flush cache file");
      fail();
    }
    else {
      bool eof = false;
      for (int i=0; i<PARALLEL_WRITES && !eof; i++)
        eof = do_write();
    }
    if (!outstanding_writes) // nothing to do
      ok();
  }

  ~write_obj() {}
};

void
lbfs_write (str fn, nfs_fh3 fh, uint64 size, fattr3 fa, ref<server> srv,
            AUTH *a, write_obj::cb_t cb)
{
  vNew write_obj (fn, fh, size, fa, srv, a, cb);
}
