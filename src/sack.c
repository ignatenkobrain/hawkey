#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

// libsolv
#include "solv/pool.h"
#include "solv/poolarch.h"
#include "solv/repo.h"
#include "solv/repo_deltainfoxml.h"
#include "solv/repo_repomdxml.h"
#include "solv/repo_rpmmd.h"
#include "solv/repo_rpmdb.h"
#include "solv/repo_solv.h"
#include "solv/repo_write.h"
#include "solv/solv_xfopen.h"
#include "solv/solver.h"
#include "solv/solverdebug.h"

// hawkey
#include "errno.h"
#include "iutil.h"
#include "package_internal.h"
#include "repo_internal.h"
#include "sack_internal.h"

#define DEFAULT_CACHE_ROOT "/var/cache/hawkey"
#define DEFAULT_CACHE_USER "/var/tmp/hawkey"

static int
current_rpmdb_checksum(unsigned char csout[CHKSUM_BYTES])
{
    FILE *fp_rpmdb = fopen(HY_SYSTEM_RPMDB, "r");
    int ret = 0;

    if (!fp_rpmdb || checksum_stat(csout, fp_rpmdb))
	ret = 1;
    if (fp_rpmdb)
	fclose(fp_rpmdb);
    return ret;
}

static int
can_use_rpmdb_cache(FILE *fp_solv, unsigned char cs[CHKSUM_BYTES])
{
    unsigned char cs_cache[CHKSUM_BYTES];

    if (fp_solv &&
	!checksum_read(cs_cache, fp_solv) &&
	!checksum_cmp(cs_cache, cs))
	return 1;

    return 0;
}

static int
can_use_repomd_cache(FILE *fp_solv, unsigned char cs_repomd[CHKSUM_BYTES])
{
    unsigned char cs_cache[CHKSUM_BYTES];

    if (fp_solv &&
	!checksum_read(cs_cache, fp_solv) &&
	!checksum_cmp(cs_cache, cs_repomd))
	return 1;

    return 0;
}

static Repo *
get_cmdline_repo(HySack sack)
{
    Pool *pool = sack->pool;
    Repo *repo;
    int repoid;

    FOR_REPOS(repoid, repo) {
	if (!strcmp(repo->name, HY_CMDLINE_REPO_NAME))
	    return repo;
    }
    return NULL;
}

int
setarch(HySack sack, const char *arch)
{
    Pool *pool = sack_pool(sack);
    struct utsname un;

    if (arch == NULL) {
	if (uname(&un))
	    {
		perror(__func__);
		exit(1);
	    }
	arch = un.machine;
    }
    HY_LOG_INFO("Architecture is: %s", arch);
    pool_setarch(pool, arch);
    if (!strcmp(arch, "noarch"))
	return 0; // noarch never fails

    /* pool_setarch() doesn't tell us when it defaulted to 'noarch' but we
       consider it a failure. the only way to find out is count the
       architectures known to the Pool. */
    int count = 0;
    for (Id id = 0; id <= pool->lastarch; ++id)
	if (pool->id2arch[id])
	    count++;
    if (count < 2)
	return 1;
    return 0;
}

static void
log_cb(Pool *pool, void *cb_data, int type, const char *buf)
{
    HySack sack = cb_data;

    if (sack->log_out == NULL) {
	const char *fn = pool_tmpjoin(pool, sack->cache_dir, "/hawkey.log", NULL);

	sack->log_out = fopen(fn, "a");
	if (sack->log_out)
	    HY_LOG_INFO("Started...", sack);
    }
    if (fwrite(buf, strlen(buf), 1, sack->log_out) < 1)
	fprintf(stderr, "logging is broken!?\n");
    fflush(sack->log_out);
}

static int
load_ext(HySack sack, HyRepo hrepo, int which_repodata,
	 const char *suffix, int which_filename,
	 int (*cb)(Repo *, FILE *))
{
    int ret = 0;
    Repo *repo = hrepo->libsolv_repo;
    const char *name = repo->name;
    const char *fn = hy_repo_get_string(hrepo, which_filename);
    FILE *fp;
    int ret_misc;
    int done = 0;

    if (fn == NULL)
	return 1;

    char *fn_cache =  hy_sack_give_cache_fn(sack, name, suffix);
    fp = fopen(fn_cache, "r");
    assert(hrepo->checksum);
    if (can_use_repomd_cache(fp, hrepo->checksum)) {
	HY_LOG_INFO("%s: using cache file: %s", __func__, fn_cache);
	ret_misc = repo_add_solv(repo, fp, REPO_EXTEND_SOLVABLES);
	assert(ret_misc == 0);
	ret |= ret_misc;
	if (ret == 0)
	    repo_update_state(hrepo, which_repodata, _HY_LOADED_CACHE);
	done = 1;
    }
    solv_free(fn_cache);
    if (fp)
	fclose(fp);
    if (done)
	return 0;

    fp = solv_xfopen(fn, "r");
    if (fp == NULL) {
	HY_LOG_ERROR("%s: failed to open: %s", __func__, fn);
	ret |= 1;
	goto finish;
    }
    HY_LOG_INFO("%s: loading: %s", __func__, fn);

    int previous_last = repo->nrepodata - 1;
    ret_misc = cb(repo, fp);
    fclose(fp);
    assert(ret_misc == 0);
    ret |= ret_misc;
    if (ret == 0)
	repo_update_state(hrepo, which_repodata, _HY_LOADED_FETCH);
    if (!ret_misc) {
	assert(previous_last == repo->nrepodata - 2);
	repo_set_repodata(hrepo, which_repodata, repo->nrepodata - 1);
    }
 finish:
    sack->provides_ready = 0;
    return ret;
}

static int
load_filelists_cb(Repo *repo, FILE *fp) {
    return repo_add_rpmmd(repo, fp, "FL", REPO_EXTEND_SOLVABLES);
}

static int
load_presto_cb(Repo *repo, FILE *fp) {
    return repo_add_deltainfoxml(repo, fp, 0);
}

static int
write_main(HySack sack, HyRepo hrepo)
{
    Repo *repo = hrepo->libsolv_repo;
    const char *name = repo->name;

    HY_LOG_INFO("caching repo: %s", name);
    assert(strcmp(name, HY_CMDLINE_REPO_NAME));
    assert(hrepo->checksum);

    char *fn = hy_sack_give_cache_fn(sack, name, NULL);
    FILE *fp = fopen(fn, "w+");
    solv_free(fn);
    if (!fp) {
	perror(__func__);
	return 1;
    }
    repo_write(repo, fp);
    checksum_write(hrepo->checksum, fp);
    fclose(fp);
    hrepo->state_main = _HY_WRITTEN;
    return 0;
}

static int
write_ext(HySack sack, HyRepo hrepo, int which_repodata, const char *suffix)
{
    Repo *repo = hrepo->libsolv_repo;
    int ret = 0;
    const char *name = repo->name;

    Id repodata = repo_get_repodata(hrepo, which_repodata);
    assert(repodata);
    Repodata *data = repo_id2repodata(repo, repodata);
    char *fn = hy_sack_give_cache_fn(sack, name, suffix);
    FILE *fp = fopen(fn, "w+");
    HY_LOG_INFO("%s: storing %s to: %s", __func__, repo->name, fn);
    solv_free(fn);
    assert(data);
    assert(hrepo->checksum);
    ret |= repodata_write(data, fp);
    ret |= checksum_write(hrepo->checksum, fp);

    fclose(fp);
    if (ret == 0)
	repo_update_state(hrepo, which_repodata, _HY_WRITTEN);
    return ret;
}

static int
load_yum_repo(HySack sack, HyRepo hrepo)
{
    int retval = 0;
    Pool *pool = sack->pool;
    const char *name = hy_repo_get_string(hrepo, HY_REPO_NAME);
    Repo *repo = repo_create(pool, name);
    const char *fn_repomd = hy_repo_get_string(hrepo, HY_REPO_MD_FN);
    char *fn_cache = hy_sack_give_cache_fn(sack, name, NULL);

    FILE *fp_cache = fopen(fn_cache, "r");
    FILE *fp_repomd = fopen(fn_repomd, "r");
    if (fp_repomd == NULL) {
	HY_LOG_ERROR("can not read repomd %s: %s", fn_repomd, strerror(errno));
	retval = 1;
	goto finish;
    }
    checksum_fp(hrepo->checksum, fp_repomd);

    assert(hrepo->state_main == _HY_NEW);
    if (can_use_repomd_cache(fp_cache, hrepo->checksum)) {
	HY_LOG_INFO("using cached %s", name);
	if (repo_add_solv(repo, fp_cache, 0))
	    assert(0);
	hrepo->state_main = _HY_LOADED_CACHE;
    } else {
	FILE *fp_primary = solv_xfopen(hy_repo_get_string(hrepo, HY_REPO_PRIMARY_FN), "r");

	if (!(fp_repomd && fp_primary))
	    assert(0);

	HY_LOG_INFO("fetching %s", name);
	repo_add_repomdxml(repo, fp_repomd, 0);
	repo_add_rpmmd(repo, fp_primary, 0, 0);

	fclose(fp_primary);
	hrepo->state_main = _HY_LOADED_FETCH;
    }

 finish:
    if (fp_cache)
	fclose(fp_cache);
    if (fp_repomd)
	fclose(fp_repomd);
    solv_free(fn_cache);

    if (retval == 0) {
	repo->appdata = hy_repo_link(hrepo);
	hrepo->libsolv_repo = repo;
	sack->provides_ready = 0;
    } else
	repo_free(repo, 1);
    return retval;
}

/**
 * Creates a new package sack, the fundamental hawkey structure.
 *
 * If 'cache_path' is not NULL it is used to override the default filesystem
 * location where hawkey will store its metadata cache and log file.
 *
 * 'arch' specifies the architecture. NULL value causes autodetection.
 */
HySack
hy_sack_create(const char *cache_path, const char *arch)
{
    HySack sack = solv_calloc(1, sizeof(*sack));
    Pool *pool = pool_create();

    sack->pool = pool;

    if (cache_path != NULL) {
	sack->cache_dir = solv_strdup(cache_path);
    } else if (geteuid()) {
	char *username = this_username();
	char *path = pool_tmpjoin(pool, DEFAULT_CACHE_USER, "-", username);
	path = pool_tmpappend(pool, path, "-", "XXXXXX");
	sack->cache_dir = solv_strdup(path);
	solv_free(username);
    } else
	sack->cache_dir = solv_strdup(DEFAULT_CACHE_ROOT);

    int ret = mkcachedir(sack->cache_dir);
    if (ret) {
	hy_errno = HY_E_IO;
	goto fail;
    }
    queue_init(&sack->installonly);

    /* logging up after this*/
    pool_setdebugcallback(pool, log_cb, sack);
    pool_setdebugmask(pool,
		      SOLV_ERROR | SOLV_FATAL | SOLV_WARN | SOLV_DEBUG_RESULT |
		      HY_LL_INFO | HY_LL_ERROR);

    if (setarch(sack, arch)) {
	hy_errno = HY_E_ARCH;
	goto fail;
    }

    return sack;
 fail:
    hy_sack_free(sack);
    return NULL;
}

void
hy_sack_free(HySack sack)
{
    Pool *pool = sack->pool;
    Repo *repo;
    int i;

    FOR_REPOS(i, repo) {
	HyRepo hrepo = repo->appdata;
	if (hrepo == NULL)
	    continue;
	hy_repo_free(hrepo);
    }
    if (sack->log_out) {
	HY_LOG_INFO("Finished.", sack);
	fclose(sack->log_out);
    }
    solv_free(sack->cache_dir);
    queue_free(&sack->installonly);
    pool_free(sack->pool);
    solv_free(sack);
}

char *
hy_sack_give_cache_fn(HySack sack, const char *reponame, const char *ext)
{
    assert(reponame);
    char *fn = solv_dupjoin(sack->cache_dir, "/", reponame);
    if (ext)
	return solv_dupappend(fn, ext, ".solvx");
    return solv_dupappend(fn, ".solv", NULL);
}

const char **
hy_sack_list_arches(HySack sack)
{
    Pool *pool = sack_pool(sack);
    const int BLOCK_SIZE = 31;
    int c = 0;
    const char **ss = NULL;

    if (!(pool->id2arch && pool->lastarch))
	return NULL;

    for (Id id = 0; id <= pool->lastarch; ++id) {
	if (!pool->id2arch[id])
	    continue;
	ss = solv_extend(ss, c, 1, sizeof(char*), BLOCK_SIZE);
	ss[c++] = pool_id2str(pool, id);
    }
    ss = solv_extend(ss, c, 1, sizeof(char*), BLOCK_SIZE);
    ss[c++] = NULL;
    return ss;
}

void
hy_sack_set_installonly(HySack sack, const char **installonly)
{
    const char *name;
    while ((name = *installonly++) != NULL)
	queue_pushunique(&sack->installonly, pool_str2id(sack->pool, name, 1));
}

/**
 * Creates repo for command line rpms.
 *
 * Does nothing if one already created.
 */
void
hy_sack_create_cmdline_repo(HySack sack)
{
    if (get_cmdline_repo(sack) == NULL)
	repo_create(sack->pool, HY_CMDLINE_REPO_NAME);
}

/**
 * Adds the given .rpm file to the command line repo.
 */
HyPackage
hy_sack_add_cmdline_package(HySack sack, const char *fn)
{
    Repo *repo = get_cmdline_repo(sack);
    Id p;

    assert(repo);
    if (!is_readable_rpm(fn)) {
	HY_LOG_ERROR("not a readable RPM file: %s, skipping", fn);
	return NULL;
    }
    p = repo_add_rpm(repo, fn, REPO_REUSE_REPODATA|REPO_NO_INTERNALIZE);
    sack->provides_ready = 0;    /* triggers internalizing later */
    return package_create(sack->pool, p);
}

int
hy_sack_count(HySack sack)
{
    // subtract two for meta solvable and system solvable, see pool_create()
    return sack_pool(sack)->nsolvables - 2;
}

int
hy_sack_load_system_repo(HySack sack, HyRepo a_hrepo, int flags)
{
    Pool *pool = sack->pool;
    char *cache_fn = hy_sack_give_cache_fn(sack, HY_SYSTEM_REPO_NAME, NULL);
    FILE *cache_fp = fopen(cache_fn, "r");
    int rc, ret = 0;
    HyRepo hrepo = a_hrepo ? a_hrepo : hy_repo_create();

    solv_free(cache_fn);
    hy_repo_set_string(hrepo, HY_REPO_NAME, HY_SYSTEM_REPO_NAME);

    rc = current_rpmdb_checksum(hrepo->checksum);
    if (rc) {
	ret = HY_E_IO;
	goto finish;
    }

    Repo *repo = repo_create(pool, HY_SYSTEM_REPO_NAME);
    if (can_use_rpmdb_cache(cache_fp, hrepo->checksum)) {
	HY_LOG_INFO("using cached rpmdb");
	rc = repo_add_solv(repo, cache_fp, 0);
	if (!rc)
	    hrepo->state_main = _HY_LOADED_CACHE;
    } else {
	HY_LOG_INFO("fetching rpmdb");
	rc = repo_add_rpmdb(repo, 0, 0,
			    REPO_REUSE_REPODATA | RPM_ADD_WITH_HDRID);
	if (!rc)
	    hrepo->state_main = _HY_LOADED_FETCH;
    }
    if (rc) {
	repo_free(repo, 1);
	ret = HY_E_IO;
	goto finish;
    }
    if (cache_fp)
	fclose(cache_fp);

    /* we managed the hardest part, setup the bidirectional pointers etc. */
    pool_set_installed(pool, repo);
    repo->appdata = hrepo;
    hrepo->libsolv_repo = repo;
    sack->provides_ready = 0;

    const int build_cache = flags & HY_BUILD_CACHE;
    if (hrepo->state_main == _HY_LOADED_FETCH && build_cache) {
	rc = write_main(sack, hrepo);
	if (rc) {
	    ret = HY_E_CACHE_WRITE;
	    goto finish;
	}
    }

 finish:
    if (a_hrepo == NULL)
	hy_repo_free(hrepo);
    return ret;
}

int
hy_sack_load_yum_repo(HySack sack, HyRepo repo, int flags)
{
    const int build_cache = flags & HY_BUILD_CACHE;
    int retval = load_yum_repo(sack, repo);
    if (retval)
	return retval;
    if (repo->state_main == _HY_LOADED_FETCH && build_cache) {
	retval = write_main(sack, repo);
	if (retval)
	    return retval;
    }
    if (flags & HY_LOAD_FILELISTS) {
	retval = load_ext(sack, repo, _HY_REPODATA_FILENAMES,
			  HY_EXT_FILENAMES, HY_REPO_FILELISTS_FN,
			  load_filelists_cb);
	if (retval)
	    return retval;
	if (repo->state_filelists == _HY_LOADED_FETCH && build_cache) {
	    retval = write_ext(sack, repo, _HY_REPODATA_FILENAMES,
			       HY_EXT_FILENAMES);
	    if (retval)
		return retval;
	}
    }
    if (flags & HY_LOAD_PRESTO) {
	retval = load_ext(sack, repo, _HY_REPODATA_PRESTO,
			  HY_EXT_PRESTO, HY_REPO_PRESTO_FN,
			  load_presto_cb);
	if (retval)
	    return retval;
	if (repo->state_presto == _HY_LOADED_FETCH && build_cache)
	    retval = write_ext(sack, repo, _HY_REPODATA_PRESTO, HY_EXT_PRESTO);
    }
    return retval;
}

void
sack_make_provides_ready(HySack sack)
{
    if (!sack->provides_ready) {
	pool_addfileprovides(sack->pool);
	pool_createwhatprovides(sack->pool);
	sack->provides_ready = 1;
    }
}

// internal

void
sack_log(HySack sack, int level, const char *format, ...)
{
    Pool *pool = sack_pool(sack);
    char buf[1024];
    va_list args;
    time_t t = time(NULL);
    struct tm tm;
    char timestr[26];

    localtime_r(&t, &tm);
    strftime(timestr, 26, "%b-%d %H:%M:%S ", &tm);
    const char *pref_format = pool_tmpjoin(pool, "hy: ", timestr, format);
    pref_format = pool_tmpappend(pool, pref_format, "\n", NULL);

    va_start(args, format);
    vsnprintf(buf, sizeof(buf), pref_format, args);
    va_end(args);
    POOL_DEBUG(level, buf);
}

/**
 * Put all solvables named 'name' into the queue 'same'.
 *
 * Futher, if arch is nonzero only solvables with a matching arch are added to
 * the queue.
 *
 */
void
sack_same_names(HySack sack, Id name, Id arch, Queue *same)
{
    Pool *pool = sack->pool;
    Id p, pp;

    sack_make_provides_ready(sack);
    FOR_PROVIDES(p, pp, name) {
	Solvable *s = pool_id2solvable(pool, p);
	if (s->name == name)
	    if (!arch || s->arch == arch)
		queue_push(same, p);
    }
}
