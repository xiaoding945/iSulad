/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: gaohuatao
 * Create: 2020-11-09
 * Description: provide CNI network manager function definition
 ******************************************************************************/
#include "manager.h"

#include <pthread.h>
#include <sys/types.h>

#include "isula_libutils/log.h"
#include "isula_libutils/cni_net_conf.h"
#include "isula_libutils/cni_net_conf_list.h"
#include "utils.h"
#include "libcni_utils.h"
#include "libcni_types.h"

#define LO_IFNAME "lo"
#define CNI_CONF_ARGS_DEFAULT_LEN 4
#define MAC_ADDR_VALID_CHARS "^([a-f0-9A-F]{2}[:-]){5}(a-f0-9A-F]{2})$"

typedef int (*annotation_add_args_t)(const char *, const char *, size_t, struct runtime_conf *);
typedef int (*annotation_add_json_t)(const char *, char **);

struct anno_registry_conf_args {
    char *name;
    annotation_add_args_t ops;
};

struct anno_registry_conf_json {
    char *name;
    annotation_add_json_t ops;
};

typedef struct cni_manager_store_t {
    char *conf_path;
    char **bin_paths;
    size_t bin_paths_len;
    char *cache_dir;
    char *loopback_conf_str;
} cni_manager_store_t;

typedef struct cni_manager_network_conf_list_t {
    struct cni_network_list_conf **conflist;
    size_t conflist_len;
    pthread_rwlock_t rwlock;
} cni_manager_network_conf_list_t;

static cni_manager_store_t g_cni_manager = {
    .loopback_conf_str = "{\"cniVersion\": \"0.3.0\", \"name\": \"cni-loopback\","
    "\"plugins\":[{\"type\": \"loopback\" }]}",
};

static cni_manager_network_conf_list_t g_conflists;

static int ip_inject(const char *key, const char *value, size_t cnt, struct runtime_conf *rt)
{
    uint8_t *ip = NULL;
    size_t ip_len = 0;
    int ret = 0;
    int nret = 0;

    if (rt == NULL || key == NULL || value == NULL) {
        return -1;
    }

    nret = parse_ip_from_str(value, &ip, &ip_len);
    if (nret != 0 || ip == NULL || ip_len == 0) {
        ERROR("Parse IP %s failed", value);
        ret = -1;
        goto out;
    }

    rt->args[cnt][0] = util_strdup_s(key);
    rt->args[cnt][1] = util_strdup_s(value);

out:
    free(ip);
    return ret;
}

static int mac_inject(const char *key, const char *value, size_t cnt, struct runtime_conf *rt)
{
    int ret = 0;

    if (rt == NULL || key == NULL || value == NULL) {
        return -1;
    }

    if (util_reg_match(MAC_ADDR_VALID_CHARS, value) != 0) {
        ERROR("MAC addr: %s is not valid", value);
        ret = -1;
        goto out;
    }

    rt->args[cnt][0] = util_strdup_s(key);
    rt->args[cnt][1] = util_strdup_s(value);

out:
    return ret;
}

static int bandwidth_inject(const char *key, const char *value, size_t cnt, struct runtime_conf *rt)
{
    return 0;
}

static int port_mappings_inject(const char *key, const char *value, size_t cnt, struct runtime_conf *rt)
{
    return 0;
}

static int ip_ranges_inject(const char *key, const char *value, size_t cnt, struct runtime_conf *rt)
{
    return 0;
}

static struct anno_registry_conf_args g_registrant_args[] = {
    {.name = "IP", .ops = ip_inject},
    {.name = "MAC", .ops = mac_inject},
    {.name = "bandwidth", .ops = bandwidth_inject},
    {.name = "portMappings", .ops = port_mappings_inject},
    {.name = "ipRanges", .ops = ip_ranges_inject}
};

static struct anno_registry_conf_json g_registrant_json[] = {
};

static const size_t g_numregistrants_args = sizeof(g_registrant_args) / sizeof(struct anno_registry_conf_args);
static const size_t g_numregistrants_json = sizeof(g_registrant_json) / sizeof(struct anno_registry_conf_json);

static inline bool conflists_wrlock()
{
    int nret = 0;

    nret = pthread_rwlock_wrlock(&g_conflists.rwlock);
    if (nret != 0) {
        ERROR("Lock conflists memory store failed: %s", strerror(nret));
        return false;
    }

    return true;
}

static inline bool conflists_rdlock()
{
    int nret = 0;

    nret = pthread_rwlock_rdlock(&g_conflists.rwlock);
    if (nret != 0) {
        ERROR("Lock conflists memory store failed: %s", strerror(nret));
        return false;
    }

    return true;
}

static inline void conflists_unlock()
{
    int nret = 0;

    nret = pthread_rwlock_unlock(&g_conflists.rwlock);
    if (nret != 0) {
        FATAL("Unlock driver memory store failed: %s", strerror(nret));
    }
}

int cni_manager_store_init(const char *cache_dir, const char *conf_path, const char* const *bin_paths,
                           size_t bin_paths_len)
{
    int ret = 0;
    if (conf_path == NULL) {
        ERROR("Invalid input param");
        return -1;
    }

    if (!cni_module_init(cache_dir, bin_paths, bin_paths_len)) {
        ERROR("Init libcni module failed");
        ret = -1;
        goto out;
    }

    if (pthread_rwlock_init(&(g_conflists.rwlock), NULL) != 0) {
        ERROR("Failed to init global conflists rwlock");
        ret = -1;
        goto out;
    }

    g_cni_manager.conf_path = util_strdup_s(conf_path);
    g_cni_manager.cache_dir = util_strdup_s(cache_dir);

out:
    return ret;
}

static int load_cni_config_file_list(const char *fname, struct cni_network_list_conf **n_list)
{
    int ret = 0;
    struct cni_network_conf *n_conf = NULL;

    if (fname == NULL || n_list == NULL) {
        ERROR("Invalid NULL params");
        return -1;
    }

    if (util_has_suffix(fname, ".conflist")) {
        if (cni_conflist_from_file(fname, n_list)) {
            ERROR("Error loading CNI config list file %s", fname);
            ret = -1;
            goto out;
        }
    } else {
        if (cni_conf_from_file(fname, &n_conf)) {
            ERROR("Error loading CNI config file %s", fname);
            ret = -1;
            goto out;
        }

        if (!util_valid_str(n_conf->type)) {
            ERROR("Error loading CNI config file %s: no 'type'; perhaps this is a .conflist?", fname);
            ret = -1;
            goto out;
        }

        if (cni_conflist_from_conf(n_conf, n_list) != 0) {
            ERROR("Error converting CNI config file %s to list", fname);
            ret = -1;
            goto out;
        }
    }

out:
    if (n_conf != NULL) {
        free_cni_network_conf(n_conf);
    }

    return ret;
}

// Try my best to load file, when error occured, just skip and continue
static int cni_manager_update_conflist_from_files(cni_manager_network_conf_list_t *store, const char **files,
                                                  size_t length)
{
    int ret = 0;
    size_t i = 0;
    char *fpath = NULL;

    if (g_cni_manager.conf_path == NULL) {
        ERROR("CNI conf path is null");
        return -1;
    }

    for (i = 0; i < length; i++) {
        struct cni_network_list_conf *n_list = NULL;

        UTIL_FREE_AND_SET_NULL(fpath);
        fpath = util_path_join(g_cni_manager.conf_path, files[i]);
        if (fpath == NULL) {
            ERROR("Failed to get CNI conf file:%s full path", files[i]);
            ret = -1;
            goto out;
        }

        if (load_cni_config_file_list(fpath, &n_list) != 0) {
            WARN("Load cni network conflist from file failed");
            continue;
        }

        if (n_list == NULL || n_list->plugin_len == 0) {
            WARN("CNI config list %s has no networks, skipping", files[i]);
            free_cni_network_list_conf(n_list);
            n_list = NULL;
            continue;
        }

        store->conflist[i] = n_list;
        n_list = NULL;
        store->conflist_len++;
    }

out:
    free(fpath);
    return ret;
}

static void free_conflists_data(struct cni_network_list_conf **conflist, size_t len)
{
    size_t i = 0;

    if (conflist == NULL) {
        return;
    }

    for (i = 0; i < len; i++) {
        free_cni_network_list_conf(conflist[i]);
        conflist[i] = NULL;
    }
    free(conflist);
}

static int update_conflist_with_lock(size_t new_length, const char **files)
{
    int ret = 0;
    size_t i = 0;
    cni_manager_network_conf_list_t tmp_conflists = { 0 };


    tmp_conflists.conflist = (struct cni_network_list_conf **)util_smart_calloc_s(sizeof(
                                                                                      struct cni_network_list_conf *) * new_length, 1);
    if (tmp_conflists.conflist == NULL) {
        ERROR("Out of memory, cannot allocate mem to store conflists");
        return -1;
    }

    tmp_conflists.conflist_len = 0;
    if (cni_manager_update_conflist_from_files(&tmp_conflists, files, new_length) != 0) {
        ERROR("Update conflist from files failed");
        ret = -1;
        goto out;
    }

    if (!conflists_wrlock()) {
        ERROR("Lock conflists store failed");
        ret = -1;
        goto out;
    }

    for (i = 0; i < g_conflists.conflist_len; i++) {
        free_cni_network_list_conf(g_conflists.conflist[i]);
    }
    free(g_conflists.conflist);

    g_conflists.conflist = tmp_conflists.conflist;
    g_conflists.conflist_len = tmp_conflists.conflist_len;
    conflists_unlock();

    tmp_conflists.conflist = NULL;
    tmp_conflists.conflist_len = 0;

out:
    free_conflists_data(tmp_conflists.conflist, tmp_conflists.conflist_len);
    return ret;
}

// Just update local data from dir
int cni_update_confist_from_dir()
{
    int ret = 0;
    size_t length = 0;
    const char *exts[] = { ".conf", ".conflist", ".json" };
    char **files = NULL;

    if (g_cni_manager.conf_path == NULL) {
        ERROR("CNI conf dir is NULL");
        return -1;
    }

    if (cni_conf_files(g_cni_manager.conf_path, exts, sizeof(exts) / sizeof(char *), &files) != 0) {
        ERROR("Get conf files from dir:%s failed", g_cni_manager.conf_path);
        ret = -1;
        goto out;
    }

    length = util_array_len((const char **)files);
    if (length == 0) {
        ERROR("No network conf files found");
        ret = -1;
        goto out;
    }

    if (update_conflist_with_lock(length, (const char **)files) != 0) {
        ERROR("Reload conflists data from conf dir failed");
        ret = -1;
        goto out;
    }

out:
    util_free_array(files);
    return ret;
}

int cni_get_conflist_from_dir(struct cni_network_list_conf ***store, size_t *res_len)
{
    int ret = 0;
    size_t i = 0;

    if (store == NULL || res_len == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    if (!conflists_rdlock()) {
        ERROR("Lock conflist data failed");
        return -1;
    }

    *res_len = 0;
    *store = (struct cni_network_list_conf **)util_smart_calloc_s(sizeof(struct cni_network_list_conf *) *
                                                                  g_conflists.conflist_len, 1);
    if (*store == NULL) {
        ERROR("Out of memory");
        ret = -1;
        goto out;
    }

    for (i = 0; i < g_conflists.conflist_len; i++) {
        struct cni_network_list_conf *list = NULL;
        list = util_smart_calloc_s(sizeof(struct cni_network_list_conf), 1);
        if (list == NULL) {
            ERROR("Out of momory, cannot allocate conflist store");
            ret = -1;
            goto free_out;
        }

        list->plugin_len = g_conflists.conflist[i]->plugin_len;
        list->name = util_strdup_s(g_conflists.conflist[i]->name);
        list->first_plugin_type = util_strdup_s(g_conflists.conflist[i]->first_plugin_type);
        list->first_plugin_name = util_strdup_s(g_conflists.conflist[i]->first_plugin_name);
        list->bytes = util_strdup_s(g_conflists.conflist[i]->bytes);
        (*store)[i] = list;
        (*res_len)++;
    }
    goto out;

free_out:
    free_conflists_data(*store, *res_len);
    *store = NULL;
    *res_len = 0;
out:
    conflists_unlock();
    return ret;
}

static struct runtime_conf *build_loopback_runtime_conf(const char *cid, const char *netns_path)
{
    struct runtime_conf *rt = NULL;

    if (cid == NULL || netns_path == NULL) {
        ERROR("Invalid input params");
        return NULL;
    }

    rt = util_smart_calloc_s(sizeof(struct runtime_conf), 1);
    if (rt == NULL) {
        ERROR("Out of memory");
        goto out;
    }

    rt->container_id = util_strdup_s(cid);
    rt->netns = util_strdup_s(netns_path);
    rt->ifname = util_strdup_s(LO_IFNAME);

out:
    return rt;
}

// inject runtime_conf args from annotation
static int inject_annotations_args(map_t *annotations, struct runtime_conf *rt)
{
    int ret = 0;
    size_t i = 0;
    size_t cnt = CNI_CONF_ARGS_DEFAULT_LEN;
    char *value = NULL;

    if (annotations == NULL) {
        return -1;
    }

    for (i = 0; i < g_numregistrants_args && cnt < rt->args_len; i++) {
        value = map_search(annotations, (void *)g_registrant_args[i].name);
        if (value == NULL) {
            DEBUG("This key:%s is not surpported", g_registrant_args[i].name);
            continue;
        }

        if (g_registrant_args[i].ops(g_registrant_args[i].name, value, cnt, rt) != 0) {
            ERROR("The format of annotation is not right with key:%s, value:%s", g_registrant_args[i].name, value);
            ret = -1;
            goto out;
        }
        cnt++;
    }

out:
    return ret;
}

// inject cni net conflist json from annotation
static int inject_annotations_json(map_t *annotations, char **net_list_conf_str)
{
    int ret = 0;
    size_t i = 0;
    char *value = NULL;

    if (annotations == NULL || net_list_conf_str == NULL) {
        ERROR("Invalid input param");
        return -1;
    }

    for (i = 0; i < g_numregistrants_json; i++) {
        value = map_search(annotations, (void *)g_registrant_json[i].name);
        if (value == NULL) {
            DEBUG("This key:%s is not surpported", g_registrant_json[i].name);
            continue;
        }

        if (g_registrant_json[i].ops(value, net_list_conf_str) != 0) {
            ERROR("The format of annotation is not right with key:%s, value:%s", g_registrant_json[i].name, value);
            ret = -1;
            goto out;
        }
    }

out:
    return ret;
}

static struct runtime_conf *build_cni_runtime_conf(struct cni_manager *manager)
{
    struct runtime_conf *rt = NULL;
    size_t args_len = CNI_CONF_ARGS_DEFAULT_LEN;
    size_t anno_len = 0;

    rt = (struct runtime_conf *)util_smart_calloc_s(sizeof(struct runtime_conf), 1);
    if (rt == NULL) {
        ERROR("Out of memory");
        return NULL;
    }

    rt->container_id = util_strdup_s(manager->id);
    rt->netns = util_strdup_s(manager->netns_path);
    rt->ifname = util_strdup_s(manager->ifname);

    anno_len = map_size(manager->annotations);
    if (anno_len > g_numregistrants_args) {
        ERROR("The annotation numbers is too larger, expecting max numbers:%zu", g_numregistrants_args);
        goto free_out;
    }

    args_len += anno_len;
    rt->args = (char *(*)[2])util_smart_calloc_s(sizeof(char *) * 2 * args_len, 1);
    if (rt->args == NULL) {
        ERROR("Out of memory");
        goto free_out;
    }

    rt->args_len = args_len;
    rt->args[0][0] = util_strdup_s("IgnoreUnknown");
    rt->args[0][1] = util_strdup_s("1");
    rt->args[1][0] = util_strdup_s("K8S_POD_NAMESPACE");
    rt->args[1][1] = util_strdup_s(manager->namespace);
    rt->args[2][0] = util_strdup_s("K8S_POD_NAME");
    rt->args[2][1] = util_strdup_s(manager->name);
    rt->args[3][0] = util_strdup_s("K8S_POD_INFRA_CONTAINER_ID");
    rt->args[3][1] = util_strdup_s(manager->id);

    if (inject_annotations_args(manager->annotations, rt) != 0) {
        ERROR("Inject annotations failed");
        goto free_out;
    }

    return rt;

free_out:
    free_runtime_conf(rt);
    return NULL;
}

// setup container loopback
int attach_loopback(const char *id, const char *netns)
{
    int ret = 0;
    struct runtime_conf *rc = NULL;
    struct result *lo_result = NULL;
    char *net_conf_str = NULL;

    if (id == NULL || netns == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    net_conf_str = util_strdup_s(g_cni_manager.loopback_conf_str);
    rc = build_loopback_runtime_conf(id, netns);
    if (rc == NULL) {
        ERROR("Error while adding to cni lo network");
        ret = -1;
        goto out;
    }

    if (cni_add_network_list(net_conf_str, rc, &lo_result) != 0) {
        ERROR("Add loopback network failed");
        ret = -1;
        goto out;
    }

out:
    free(net_conf_str);
    free_result(lo_result);
    free_runtime_conf(rc);
    return ret;
}

int attach_network_plane(struct cni_manager *manager, const char *net_list_conf_str)
{
    int ret = 0;
    struct runtime_conf *rc = NULL;
    struct result *net_result = NULL;
    char *net_list_conf_str_var = NULL;

    if (manager == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    net_list_conf_str_var = util_strdup_s(net_list_conf_str);
    if (net_list_conf_str_var == NULL) {
        ERROR("Dup net list conf str failed");
        ret = -1;
        goto out;
    }

    rc = build_cni_runtime_conf(manager);
    if (rc == NULL) {
        ERROR("Error building CNI runtime config");
        ret = -1;
        goto out;
    }

    if (inject_annotations_json(manager->annotations, &net_list_conf_str_var) != 0) {
        ERROR("Inject annotations to net conf json failed");
        ret = -1;
        goto out;
    }

    if (cni_add_network_list(net_list_conf_str, rc, &net_result) != 0) {
        ERROR("Add CNI network failed");
        ret = -1;
        goto out;
    }

out:
    free_result(net_result);
    free_runtime_conf(rc);
    return ret;
}

int detach_network_plane(struct cni_manager *manager, const char *net_list_conf_str)
{
    int ret = 0;
    struct runtime_conf *rc = NULL;
    char *net_list_conf_str_var = NULL;

    if (manager == NULL) {
        ERROR("Invalid input params");
        return -1;
    }

    net_list_conf_str_var = util_strdup_s(net_list_conf_str);
    if (net_list_conf_str_var == NULL) {
        ERROR("Dup net list conf str failed");
        ret = -1;
        goto out;
    }

    rc = build_cni_runtime_conf(manager);
    if (rc == NULL) {
        ERROR("Error building CNI runtime config");
        ret = -1;
        goto out;
    }

    if (inject_annotations_json(manager->annotations, &net_list_conf_str_var) != 0) {
        ERROR("Inject annotations to net conf json failed");
        ret = -1;
        goto out;
    }

    if (cni_del_network_list(net_list_conf_str, rc) != 0) {
        ERROR("Error deleting network: %s", manager->name);
        ret = -1;
        goto out;
    }

out:
    free_runtime_conf(rc);
    return ret;
}

int detach_loopback(struct cni_manager *manager)
{
    int ret = 0;
    struct runtime_conf *rc = NULL;
    char *net_list_conf_str = NULL;

    if (manager == NULL) {
        ERROR("Invalid input param");
        return -1;
    }

    net_list_conf_str = util_strdup_s(g_cni_manager.loopback_conf_str);
    rc = build_cni_runtime_conf(manager);
    if (rc == NULL) {
        ERROR("Error building CNI runtime config");
        ret = -1;
        goto out;
    }

    if (cni_del_network_list(net_list_conf_str, rc) != 0) {
        ERROR("Error deleting network: %s", manager->name);
        ret = -1;
        goto out;
    }

out:
    free(net_list_conf_str);
    free_runtime_conf(rc);
    return ret;
}

void free_cni_manager(struct cni_manager *manager)
{
    if (manager == NULL) {
        return;
    }

    UTIL_FREE_AND_SET_NULL(manager->id);
    UTIL_FREE_AND_SET_NULL(manager->ifname);
    UTIL_FREE_AND_SET_NULL(manager->name);
    UTIL_FREE_AND_SET_NULL(manager->namespace);
    UTIL_FREE_AND_SET_NULL(manager->netns_path);
    map_free(manager->annotations);
    manager->annotations = NULL;

    free(manager);
}