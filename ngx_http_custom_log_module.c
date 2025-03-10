#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <pthread.h>
#include <netinet/in.h>
#include <unistd.h>    // for sleep()
#include <stdlib.h>
#include <string.h>

#ifndef LOG_IF_LUA_SUPPORT
#define LOG_IF_LUA_SUPPORT
#endif
#ifdef LOG_IF_LUA_SUPPORT
#include <lauxlib.h>
#include "ngx_http_lua_api.h"
#endif

// --------------- Constants ---------------
#define MAX_LOG_LINE_LEN               4096
#define HEADER_USER_AGENT_LEN_LIMIT    350
#define HEADER_UNPARSED_URI_LEN_LIMIT  350
#define METHOD_NAME_LEN_LIMIT          20
#define OTHER_CUSTOM_DATA_LEN_LIMIT    100
#define HEADER_REFRERER_LEN_LIMIT      100
#define LOG_FILENAME_MAX_LEN           1024   // Increased size for filename path

// --------------- Module Location Conf ---------------
typedef struct {
    ngx_str_t  type;
    ngx_str_t  custom_data;
    int        lua;  // track if set via Lua
} ngx_http_custom_log_conf_t;

// --------------- Queue Structures ---------------
typedef struct log_queue_node_s {
    u_char data[NGX_MAX_ERROR_STR];
    u_char log_filename[LOG_FILENAME_MAX_LEN];
    size_t datasize;
    struct log_queue_node_s *next;
} log_queue_node_t;

typedef struct {
    ngx_pool_t       *pool;
    log_queue_node_t *head;
    log_queue_node_t *tail;
    size_t            size;
    pthread_mutex_t   mutex;
    pthread_t         thread;
    int               inited;
    int               stopping;
} log_queue_t;

// --------------- Global Queue Pointer (per worker) ---------------
static log_queue_t *g_queue = NULL;

// --------------- Forward Declarations ---------------
static void        *ngx_http_custom_log_create_loc_conf(ngx_conf_t *cf);
static char        *ngx_http_custom_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t    ngx_http_custom_log_handler(ngx_http_request_t *r);
static ngx_int_t    ngx_http_custom_log_init(ngx_conf_t *cf);

// For graceful shutdown:
static void         ngx_http_custom_log_exit_process(ngx_cycle_t *cycle);

static char        *ngx_http_custom_log_set_type(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t    ngx_http_custom_log_get_variable(ngx_http_request_t *r, ngx_str_t *name, ngx_str_t *value);

static unsigned char *FormattingCorrectionStrings(ngx_pool_t *pool, unsigned char *raw, size_t length);

#ifdef LOG_IF_LUA_SUPPORT
static int ngx_http_lua_custom_log_set_type(lua_State *L);
static int ngx_http_lua_custom_log_register_function(lua_State *L);
#endif

// --------------- Command Array ---------------
static ngx_command_t ngx_http_custom_log_commands[] = {
    {
        ngx_string("log_type"),
        NGX_HTTP_LOC_CONF | NGX_CONF_TAKE2,
        ngx_http_custom_log_set_type,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
    ngx_null_command
};

// --------------- Module Context ---------------
static ngx_http_module_t ngx_http_custom_log_module_ctx = {
    NULL,                        /* preconfiguration */
    ngx_http_custom_log_init,    /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_custom_log_create_loc_conf,   /* create location config */
    ngx_http_custom_log_merge_loc_conf     /* merge location config */
};

// --------------- Module Definition ---------------
ngx_module_t ngx_http_custom_log_module = {
    NGX_MODULE_V1,
    &ngx_http_custom_log_module_ctx,       /* module context */
    ngx_http_custom_log_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_http_custom_log_exit_process,      /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};

// -----------------------------------------------------------------------------
// Queue Helpers
// -----------------------------------------------------------------------------

// Push node onto queue (tail)
static void
queue_push(log_queue_t *q, log_queue_node_t *node)
{
    if (!q || !node) {
        return;
    }
    pthread_mutex_lock(&q->mutex);
    if (q->tail == NULL) {
        q->head = node;
        q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }
    q->size++;
    pthread_mutex_unlock(&q->mutex);
}

// Pop node from queue (head)
static log_queue_node_t *
queue_pop(log_queue_t *q)
{
    if (!q) {
        return NULL;
    }

    pthread_mutex_lock(&q->mutex);
    log_queue_node_t *n = q->head;
    if (n) {
        q->head = n->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        q->size--;
    }
    pthread_mutex_unlock(&q->mutex);
    return n;
}

// Flush everything in the queue
static void
queue_flush_all(log_queue_t *q)
{
    if (!q || !q->inited) {
        return;
    }

    for (;;) {
        log_queue_node_t *node = queue_pop(q);
        if (!node) {
            break;  // queue empty
        }

        // Write log data to file
        ngx_fd_t fd = ngx_open_file((char *) node->log_filename,
                                    NGX_FILE_APPEND,
                                    NGX_FILE_CREATE_OR_OPEN,
                                    NGX_FILE_DEFAULT_ACCESS);

        if (fd == NGX_INVALID_FILE) {
            // Log open error
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                          "custom_log: failed to open log file: \"%s\"", node->log_filename);
            ngx_pfree(q->pool, node);
            continue;
        }

        // Attempt partial-write loop
        ssize_t written;
        size_t total_written = 0;
        while (total_written < node->datasize) {
            written = ngx_write_fd(fd,
                                   node->data + total_written,
                                   node->datasize - total_written);
            if (written < 0) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                              "custom_log: write() failed for file \"%s\" after %z bytes",
                              node->log_filename, total_written);
                break;
            } else if (written == 0) {
                // No more could be written for some reason
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "custom_log: write() returned 0 for file \"%s\" after %z bytes",
                              node->log_filename, total_written);
                break;
            }
            total_written += written;
        }

        ngx_close_file(fd);
        // free the node
        ngx_pfree(q->pool, node);
    }
}

// The thread function: flush logs every second until stopping
static void *
log_thread_func(void *data)
{
    log_queue_t *q = (log_queue_t *)data;

    while (1) {
        // Check stopping flag
        pthread_mutex_lock(&q->mutex);
        int stop_flag = q->stopping;
        pthread_mutex_unlock(&q->mutex);

        if (stop_flag) {
            break;
        }

        // Flush any queued logs
        queue_flush_all(q);
        sleep(1);
    }

    // Final flush if desired
    queue_flush_all(q);

    // Signal thread has exited
    pthread_mutex_lock(&q->mutex);
    q->thread = 0;
    pthread_mutex_unlock(&q->mutex);

    return NULL;
}

// -----------------------------------------------------------------------------
// Handler: builds the log line, pushes to the global queue
// -----------------------------------------------------------------------------
static ngx_int_t ngx_http_custom_log_handler(ngx_http_request_t *r)
{
    ngx_http_custom_log_conf_t *llcf;
    llcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_log_module);
    if (!llcf) {
        return NGX_DECLINED;
    }
    // skip if type not set
    if (llcf->type.len == 0) {
        return NGX_DECLINED;
    }

    // gather variables
    ngx_str_t time_local        = ngx_string("time_local");
    ngx_str_t request_id        = ngx_string("request_id");
    ngx_str_t username          = ngx_string("username");
    ngx_str_t request_platform  = ngx_string("request_platform");
    ngx_str_t geoip_cc          = ngx_string("geoip2_data_country_code");
    ngx_str_t upstream_cache    = ngx_string("upstream_cache_status");
    ngx_str_t challenge_type    = ngx_string("challenge_type");

    ngx_str_t time_local_val       = ngx_null_string;
    ngx_str_t request_id_val       = ngx_null_string;
    ngx_str_t username_val         = ngx_null_string;
    ngx_str_t request_platform_val = ngx_null_string;
    ngx_str_t geoip_cc_val         = ngx_null_string;
    ngx_str_t upstream_cache_val   = ngx_null_string;
    ngx_str_t challenge_type_val   = ngx_null_string;

    (void) ngx_http_custom_log_get_variable(r, &time_local,       &time_local_val);
    (void) ngx_http_custom_log_get_variable(r, &request_id,       &request_id_val);
    (void) ngx_http_custom_log_get_variable(r, &username,         &username_val);
    (void) ngx_http_custom_log_get_variable(r, &request_platform, &request_platform_val);
    (void) ngx_http_custom_log_get_variable(r, &geoip_cc,         &geoip_cc_val);
    (void) ngx_http_custom_log_get_variable(r, &upstream_cache,   &upstream_cache_val);
    (void) ngx_http_custom_log_get_variable(r, &challenge_type,   &challenge_type_val);

    if (challenge_type_val.len == 0) {
        static ngx_str_t default_challenge_type = ngx_string("dynamic");
        challenge_type_val = default_challenge_type;
    }

    // referer / user-agent
    ngx_str_t referer_value    = ngx_null_string;
    ngx_str_t user_agent_value = ngx_null_string;
    if (r->headers_in.referer) {
        referer_value = r->headers_in.referer->value;
    }
    if (r->headers_in.user_agent) {
        user_agent_value = r->headers_in.user_agent->value;
    }

    // Trim
    if (referer_value.len > HEADER_REFRERER_LEN_LIMIT) {
        referer_value.len = HEADER_REFRERER_LEN_LIMIT;
        referer_value.data[HEADER_REFRERER_LEN_LIMIT] = '\0';
    }
    if (user_agent_value.len > HEADER_USER_AGENT_LEN_LIMIT) {
        user_agent_value.len = HEADER_USER_AGENT_LEN_LIMIT;
        user_agent_value.data[HEADER_USER_AGENT_LEN_LIMIT] = '\0';
    }
    if (r->unparsed_uri.len > HEADER_UNPARSED_URI_LEN_LIMIT) {
        r->unparsed_uri.len = HEADER_UNPARSED_URI_LEN_LIMIT;
        r->unparsed_uri.data[HEADER_UNPARSED_URI_LEN_LIMIT] = '\0';
    }
    if (r->method_name.len > METHOD_NAME_LEN_LIMIT) {
        r->method_name.len = METHOD_NAME_LEN_LIMIT;
        r->method_name.data[METHOD_NAME_LEN_LIMIT] = '\0';
    }
    if (llcf->custom_data.len > OTHER_CUSTOM_DATA_LEN_LIMIT) {
        llcf->custom_data.len = OTHER_CUSTOM_DATA_LEN_LIMIT;
        llcf->custom_data.data[OTHER_CUSTOM_DATA_LEN_LIMIT] = '\0';
    }

    // sanitize
    unsigned char *fmt = FormattingCorrectionStrings(r->pool, referer_value.data, referer_value.len);
    if (fmt) {
        referer_value.data = fmt;
        referer_value.len  = ngx_strlen(fmt);
    }

    fmt = FormattingCorrectionStrings(r->pool, user_agent_value.data, user_agent_value.len);
    if (fmt) {
        user_agent_value.data = fmt;
        user_agent_value.len  = ngx_strlen(fmt);
    }

    fmt = FormattingCorrectionStrings(r->pool, r->method_name.data, r->method_name.len);
    if (fmt) {
        r->method_name.data = fmt;
        r->method_name.len  = ngx_strlen(fmt);
    }

    fmt = FormattingCorrectionStrings(r->pool, llcf->custom_data.data, llcf->custom_data.len);
    if (fmt) {
        llcf->custom_data.data = fmt;
        llcf->custom_data.len  = ngx_strlen(fmt);
    }

    // if you want to wrap custom_data: "value"-"
    if (llcf->custom_data.len > 0) {
        // Allocate enough bytes from r->pool: original length + 3 for "\"-\"" + null-terminator
        size_t needed = llcf->custom_data.len + 3 + 1;
        u_char *p = ngx_pnalloc(r->pool, needed);
        if (p == NULL) {
            return NGX_ERROR; // handle allocation error
        }

        ngx_memzero(p, needed);
        ngx_sprintf(p, "%V\"-\"", &llcf->custom_data);

        llcf->custom_data.len  = ngx_strlen(p);
        llcf->custom_data.data = p;
    }

    // build log line
    u_char log_line[NGX_MAX_ERROR_STR];
    ngx_memzero(log_line, sizeof(log_line));
    u_char *line_end = log_line;

    if (ngx_strncmp(llcf->type.data, "challenge", 9) == 0) {
        line_end = ngx_slprintf(log_line, log_line + sizeof(log_line) - 1,
            "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"\n",
            &time_local_val,
            &request_id_val,
            &r->connection->addr_text,
            &r->method_name,
            &r->unparsed_uri,
            &challenge_type_val,
            r->headers_out.status,
            &referer_value,
            &request_platform_val,
            &geoip_cc_val,
            &llcf->custom_data,
            &user_agent_value
        );
    }
    else if (ngx_strncmp(llcf->type.data, "waf", 3) == 0) {
        line_end = ngx_slprintf(log_line, log_line + sizeof(log_line) - 1,
            "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%O\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"\n",
            &time_local_val,
            &request_id_val,
            &r->connection->addr_text,
            &r->method_name,
            &r->unparsed_uri,
            r->headers_out.status,
            r->connection->sent,
            &referer_value,
            &request_platform_val,
            &geoip_cc_val,
            &llcf->custom_data,
            &user_agent_value
        );
    }
    else {
        // fallback
        line_end = ngx_slprintf(log_line, log_line + sizeof(log_line) - 1,
            "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%O\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"\n",
            &time_local_val,
            &request_id_val,
            &r->connection->addr_text,
            &r->method_name,
            &r->unparsed_uri,
            r->headers_out.status,
            r->connection->sent,
            &referer_value,
            &request_platform_val,
            &geoip_cc_val,
            &upstream_cache_val,
            &llcf->custom_data,
            &user_agent_value
        );
    }

    // build path to log file
    ngx_str_t log_filename;
    ngx_memzero(&log_filename, sizeof(log_filename));

    ngx_str_t log_folder;
    ngx_str_t log_format;

    if (username_val.len > 0) {
        ngx_str_set(&log_folder, "/proginter/config/users/");
        ngx_str_set(&log_format, "/logs/last/%V.log");
    } else {
        ngx_str_set(&log_folder, "/var/log/nginx/");
        ngx_str_set(&log_format, "/%V.log");
    }

    size_t path_len = log_folder.len + username_val.len
                      + log_format.len + llcf->type.len + 1;
    // Check for potential overflow in qnode->log_filename
    if (path_len >= LOG_FILENAME_MAX_LEN) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "custom_log: resulting log path too long (%uz >= %d), skipping",
                      path_len, LOG_FILENAME_MAX_LEN);
        return NGX_DECLINED;
    }

    log_filename.data = ngx_pnalloc(r->pool, path_len);
    if (!log_filename.data) {
        return NGX_ERROR;
    }
    ngx_memzero(log_filename.data, path_len);

    ngx_memcpy(log_filename.data, log_folder.data, log_folder.len);
    if (username_val.len > 0) {
        ngx_memcpy(log_filename.data + log_folder.len,
                   username_val.data, username_val.len);
    }

    ngx_sprintf(log_filename.data + log_folder.len + username_val.len,
                (char *) log_format.data, &llcf->type);
    log_filename.len = ngx_strlen(log_filename.data);

    if (!g_queue) {
        return NGX_ERROR;  // shouldn't happen if module init succeeded
    }

    // Allocate a node from the global queue pool
    log_queue_node_t *qnode = ngx_pcalloc(g_queue->pool, sizeof(log_queue_node_t));
    if (!qnode) {
        return NGX_ERROR;
    }

    // Copy log line
    size_t msg_len = (size_t)(line_end - log_line);
    if (msg_len > sizeof(qnode->data)) {
        msg_len = sizeof(qnode->data);
    }
    ngx_memcpy(qnode->data, log_line, msg_len);
    qnode->datasize = msg_len;

    // Copy filename
    size_t fname_len = (log_filename.len < (LOG_FILENAME_MAX_LEN - 1))
                       ? log_filename.len
                       : (LOG_FILENAME_MAX_LEN - 1);
    ngx_memcpy(qnode->log_filename, log_filename.data, fname_len);
    qnode->log_filename[fname_len] = '\0';
    qnode->next = NULL;

    // Push to queue
    queue_push(g_queue, qnode);

    // If was set via Lua, optionally free those strings:
    if (llcf->lua) {
        ngx_pfree(r->pool, llcf->type.data);
        ngx_pfree(r->pool, llcf->custom_data.data);
        llcf->type.data        = NULL;
        llcf->type.len         = 0;
        llcf->custom_data.data = NULL;
        llcf->custom_data.len  = 0;
        llcf->lua = 0;
    }

#if !(NGX_THREADS)
    // If not using threads, flush synchronously
    queue_flush_all(g_queue);
#endif

    return NGX_OK;
}

// -----------------------------------------------------------------------------
// String Formatting Correction
// -----------------------------------------------------------------------------
static unsigned char *
FormattingCorrectionStrings(ngx_pool_t *pool, unsigned char *raw, size_t length)
{
    if (!raw || length == 0) {
        return NULL;
    }
    // +1 for null terminator
    unsigned char *newstr = ngx_pcalloc(pool, length + 1);
    if (!newstr) {
        return NULL;
    }

    size_t line_length = 0;
    for (size_t i = 0; i < length; i++) {
        if (raw[i] != '\n' && raw[i] != '\t' &&
            raw[i] != '\r' && raw[i] != '\"')
        {
            newstr[line_length++] = raw[i];
        }
    }
    newstr[line_length] = '\0';
    return newstr;
}

// -----------------------------------------------------------------------------
// Location Config
// -----------------------------------------------------------------------------
static void *
ngx_http_custom_log_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_custom_log_conf_t *conf;
    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_custom_log_conf_t));
    if (!conf) {
        return NULL;
    }
    conf->type.len        = 0;
    conf->type.data       = NULL;
    conf->custom_data.len = 0;
    conf->custom_data.data = NULL;
    conf->lua = 0;
    return conf;
}

static char *
ngx_http_custom_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_custom_log_conf_t *prev = parent;
    ngx_http_custom_log_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->type,        prev->type,        "");
    ngx_conf_merge_str_value(conf->custom_data, prev->custom_data, "");
    return NGX_CONF_OK;
}

// -----------------------------------------------------------------------------
// Post-Configuration Init
// -----------------------------------------------------------------------------
static ngx_int_t ngx_http_custom_log_init(ngx_conf_t *cf)
{
    // Create a pool that lasts the worker's lifetime
    ngx_pool_t *pool = ngx_create_pool(8192, cf->log);
    if (!pool) {
        return NGX_ERROR;
    }

    g_queue = ngx_pcalloc(pool, sizeof(log_queue_t));
    if (!g_queue) {
        ngx_destroy_pool(pool);
        return NGX_ERROR;
    }

    g_queue->pool     = pool;
    g_queue->head     = NULL;
    g_queue->tail     = NULL;
    g_queue->size     = 0;
    g_queue->inited   = 1;
    g_queue->stopping = 0;

    pthread_mutex_init(&g_queue->mutex, NULL);

    // Register the custom log handler in the log phase
    ngx_http_core_main_conf_t *cmcf;
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    if (!cmcf) {
        return NGX_ERROR;
    }

    ngx_http_handler_pt *h;
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (!h) {
        return NGX_ERROR;
    }
    *h = ngx_http_custom_log_handler;

#if (NGX_THREADS)
    // Start background flush thread
    if (pthread_create(&g_queue->thread, NULL, log_thread_func, g_queue) != 0) {
        return NGX_ERROR;
    }
#endif

#ifdef LOG_IF_LUA_SUPPORT
    // Expose Lua function: require("ngx.custom_log").log_type(...)
    if (ngx_http_lua_add_package_preload(cf, "ngx.custom_log",
                ngx_http_lua_custom_log_register_function) != NGX_OK)
    {
        return NGX_ERROR;
    }
#endif

    return NGX_OK;
}

// -----------------------------------------------------------------------------
// Graceful Shutdown (exit_process)
// -----------------------------------------------------------------------------
static void
ngx_http_custom_log_exit_process(ngx_cycle_t *cycle)
{
    if (!g_queue) {
        return;
    }

    if (g_queue->inited) {
        // Signal the flush thread to stop
        pthread_mutex_lock(&g_queue->mutex);
        g_queue->stopping = 1;
        pthread_mutex_unlock(&g_queue->mutex);

        // Wait for thread to exit (if it was started)
        if (g_queue->thread) {
            pthread_join(g_queue->thread, NULL);
            g_queue->thread = 0;
        }

        // Final flush of any leftover items
        queue_flush_all(g_queue);

        // Cleanup
        pthread_mutex_destroy(&g_queue->mutex);

        ngx_destroy_pool(g_queue->pool);
        g_queue = NULL;
    }
}

// -----------------------------------------------------------------------------
// Command Handler for 'log_type'
// -----------------------------------------------------------------------------
static char *
ngx_http_custom_log_set_type(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_custom_log_conf_t *llcf = conf;
    ngx_str_t *value = cf->args->elts;

    if (!llcf) {
        return NGX_CONF_ERROR;
    }

    // Validate the first argument
    if (ngx_strcmp(value[1].data, "dynamic")   != 0 &&
        ngx_strcmp(value[1].data, "static")    != 0 &&
        ngx_strcmp(value[1].data, "ajax")      != 0 &&
        ngx_strcmp(value[1].data, "waf")       != 0 &&
        ngx_strcmp(value[1].data, "challenge") != 0)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid log_type value \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    llcf->type        = value[1];
    llcf->custom_data = value[2];
    llcf->lua         = 0;

    return NGX_CONF_OK;
}

// -----------------------------------------------------------------------------
// Helper: Get Variable by Name
// -----------------------------------------------------------------------------
static ngx_int_t
ngx_http_custom_log_get_variable(ngx_http_request_t *r, ngx_str_t *name, ngx_str_t *value)
{
    if (!r || !name) {
        return NGX_ERROR;
    }

    ngx_uint_t key = ngx_hash_key(name->data, name->len);
    if (key == (ngx_uint_t)NGX_ERROR) {
        return NGX_ERROR;
    }

    ngx_http_variable_value_t *vv = ngx_http_get_variable(r, name, key);
    if (!vv || vv->not_found || !vv->data) {
        return NGX_ERROR;
    }

    value->len  = vv->len;
    value->data = vv->data;
    return NGX_OK;
}

// -----------------------------------------------------------------------------
// Optional Lua Support
// -----------------------------------------------------------------------------
#ifdef LOG_IF_LUA_SUPPORT

static int
ngx_http_lua_custom_log_register_function(lua_State *L)
{
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, ngx_http_lua_custom_log_set_type);
    lua_setfield(L, -2, "log_type");
    return 1;
}

static int
ngx_http_lua_custom_log_set_type(lua_State *L)
{
    if (lua_gettop(L) != 2) {
        return luaL_error(L, "exactly 2 arguments expected");
    }

    const char *type  = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);

    ngx_http_request_t *r = ngx_http_lua_get_request(L);
    if (!r) {
        return luaL_error(L, "no request object found");
    }

    ngx_http_custom_log_conf_t *llcf;
    llcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_log_module);
    if (!llcf) {
        return 0;
    }

    // copy strings into llcf
    size_t tlen = ngx_strlen(type);
    llcf->type.data = ngx_pnalloc(r->pool, tlen + 1);
    if (!llcf->type.data) {
        return luaL_error(L, "out of memory for type");
    }
    ngx_cpystrn(llcf->type.data, (u_char *)type, tlen + 1);
    llcf->type.len = tlen;

    size_t vlen = ngx_strlen(value);
    llcf->custom_data.data = ngx_pnalloc(r->pool, vlen + 1);
    if (!llcf->custom_data.data) {
        return luaL_error(L, "out of memory for custom_data");
    }
    ngx_cpystrn(llcf->custom_data.data, (u_char *)value, vlen + 1);
    llcf->custom_data.len = vlen;

    llcf->lua = 1;

    lua_pushboolean(L, 1);
    return 1;
}

#endif // LOG_IF_LUA_SUPPORT
