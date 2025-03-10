#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <pthread.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#ifndef LOG_IF_LUA_SUPPORT
#define LOG_IF_LUA_SUPPORT
#endif

#ifdef LOG_IF_LUA_SUPPORT
#include <lauxlib.h>
#include "ngx_http_lua_api.h"
#endif

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------
#define MAX_LOG_LINE_LEN             4096
#define HEADER_USER_AGENT_LEN_LIMIT  350
#define HEADER_UNPARSED_URI_LEN_LIMIT 350
#define METHOD_NAME_LEN_LIMIT        20
#define OTHER_CUSTOM_DATA_LEN_LIMIT  100
#define HEADER_REFRERER_LEN_LIMIT    100

// How big the list can grow before forcing a flush
#define MAX_LIST_BUFFER_SIZE         100

// How many milliseconds to wait before forcing a flush even if list is smaller
#define LIST_BUFFER_FLUSH_DEPLAY_MS  1000

// -----------------------------------------------------------------------------
// Location Config
// -----------------------------------------------------------------------------
typedef struct {
    ngx_str_t type;         // log_type
    ngx_str_t custom_data;
    int       lua;          // indicates if set by Lua
} ngx_http_custom_log_conf_t;

// -----------------------------------------------------------------------------
// Linked List / Shared Info Structures
// -----------------------------------------------------------------------------
typedef struct log_shared_info {
    unsigned char data[NGX_MAX_ERROR_STR];
    unsigned char log_filename[256];
    size_t        datasize;
    struct log_shared_info * volatile next;
} log_shared_info_t;

typedef struct {
    int   inited;          // 1 = valid, 0 = not inited
    log_shared_info_t *begin;
    log_shared_info_t *end;
    ngx_pool_t        *pool;
    pthread_t          pid;
    size_t             nodes_size;
} log_shared_info_head_t;

// -----------------------------------------------------------------------------
// Global pointer to the shared list head
// -----------------------------------------------------------------------------
#if (NGX_THREADS)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static log_shared_info_head_t *head = NULL;

// -----------------------------------------------------------------------------
// Forward Declarations
// -----------------------------------------------------------------------------
static void             *ngx_http_custom_log_create_loc_conf(ngx_conf_t *cf);
static char             *ngx_http_custom_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static ngx_int_t         ngx_http_custom_log_handler(ngx_http_request_t *r);
static ngx_int_t         ngx_http_custom_log_init(ngx_conf_t *cf);
static char             *ngx_http_custom_log_set_type(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t         ngx_http_custom_log_get_variable(ngx_http_request_t *r, ngx_str_t *name, ngx_str_t *value);

static void              ngx_log_fflush(log_shared_info_head_t *head);
static log_shared_info_head_t *init_info_list(ngx_conf_t *cf, log_shared_info_head_t *head);
static log_shared_info_head_t *add_info_list(log_shared_info_head_t *head, log_shared_info_t *node);
static log_shared_info_head_t *del_info_list(log_shared_info_head_t *head, log_shared_info_t *node);

static unsigned char    *FormattingCorrectionStrings(ngx_pool_t *pool, unsigned char *raw, size_t length);

#ifdef LOG_IF_LUA_SUPPORT
static int  ngx_http_lua_custom_log_set_type(lua_State * L);
static int  ngx_http_lua_custom_log_register_function(lua_State * L);
#endif

#if (NGX_THREADS)
static void *log_thread_func(void *data);
#endif

// -----------------------------------------------------------------------------
// Module Directives
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// Module Context
// -----------------------------------------------------------------------------
static ngx_http_module_t ngx_http_custom_log_module_ctx = {
    NULL,                         /* preconfiguration */
    ngx_http_custom_log_init,     /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_custom_log_create_loc_conf,   /* create location configuration */
    ngx_http_custom_log_merge_loc_conf     /* merge location configuration */
};

// -----------------------------------------------------------------------------
// Module Definition
// -----------------------------------------------------------------------------
ngx_module_t ngx_http_custom_log_module = {
    NGX_MODULE_V1,
    &ngx_http_custom_log_module_ctx,  /* module context */
    ngx_http_custom_log_commands,     /* module directives */
    NGX_HTTP_MODULE,                  /* module type */
    NULL,                             /* init master */
    NULL,                             /* init module */
    NULL,                             /* init process */
    NULL,                             /* init thread */
    NULL,                             /* exit thread */
    NULL,                             /* exit process */
    NULL,                             /* exit master */
    NGX_MODULE_V1_PADDING
};

// -----------------------------------------------------------------------------
// Flush Function: Writes & Removes items from the list
// -----------------------------------------------------------------------------
static void
ngx_log_fflush(log_shared_info_head_t *head)
{
    if (head == NULL || head->inited == 0) {
        return;
    }

    static ngx_msec_t prevcall_time = 0;

    // If we haven't reached MAX_LIST_BUFFER_SIZE, only flush if enough time has passed
    if (head->nodes_size < MAX_LIST_BUFFER_SIZE) {
        if (prevcall_time == 0) {
            prevcall_time = ngx_time();
        } else {
            ngx_msec_t currentcall_time = ngx_time();
            ngx_msec_t difftime = (currentcall_time - prevcall_time) * 1000;
            if (difftime < LIST_BUFFER_FLUSH_DEPLAY_MS) {
                // Not enough time has elapsed yet, skip flushing
                return;
            }
            prevcall_time = currentcall_time;
        }
    } else {
        // If we have >= MAX_LIST_BUFFER_SIZE, flush now and reset timer
        prevcall_time = ngx_time();
    }

#if (NGX_THREADS)
    pthread_mutex_lock(&mutex);
#endif

    log_shared_info_t *info = head->begin;

#if (NGX_THREADS)
    pthread_mutex_unlock(&mutex);
#endif

    int did_unlock = 0;

    while (info != NULL) {
        // Acquire lock to read from node safely
#if (NGX_THREADS)
        pthread_mutex_lock(&mutex);
#endif

        // Copy out data needed for write
        unsigned char line_buffer[NGX_MAX_ERROR_STR];
        ngx_memzero(line_buffer, NGX_MAX_ERROR_STR);

        size_t data_size = info->datasize;
        if (data_size > NGX_MAX_ERROR_STR) {
            data_size = NGX_MAX_ERROR_STR;
        }
        ngx_memcpy(line_buffer, info->data, data_size);

        // Copy filename
        unsigned char fname[256];
        ngx_memzero(fname, sizeof(fname));
        ngx_memcpy(fname, info->log_filename, sizeof(info->log_filename));

        log_shared_info_t *delete_node = info;
        info = info->next;

        // We'll remove from the list after writing
#if (NGX_THREADS)
        pthread_mutex_unlock(&mutex);
#endif

        // Try to open the file
        ngx_fd_t fd = ngx_open_file((char *)fname,
                                    NGX_FILE_APPEND,
                                    NGX_FILE_CREATE_OR_OPEN,
                                    NGX_FILE_DEFAULT_ACCESS);
        if (fd == NGX_INVALID_FILE) {
            // Could not open file
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                          "custom_log: failed to open \"%s\"", fname);

            // re-lock, remove node from list
#if (NGX_THREADS)
            pthread_mutex_lock(&mutex);
#endif
            del_info_list(head, delete_node);
#if (NGX_THREADS)
            pthread_mutex_unlock(&mutex);
#endif
            continue;
        }

        // Write out in a partial-write loop
        size_t total_written = 0;
        while (total_written < data_size) {
            ssize_t written = ngx_write_fd(fd,
                                           line_buffer + total_written,
                                           data_size - total_written);
            if (written <= 0) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno,
                              "custom_log: write() failed for file \"%s\"", fname);
                break;
            }
            total_written += written;
        }

        ngx_close_file(fd);

        // Now remove node from list
#if (NGX_THREADS)
        pthread_mutex_lock(&mutex);
#endif
        del_info_list(head, delete_node);
#if (NGX_THREADS)
        pthread_mutex_unlock(&mutex);
#endif
    }

    // End of flush
}

// -----------------------------------------------------------------------------
// Initialize the Linked List "head"
// -----------------------------------------------------------------------------
static log_shared_info_head_t *
init_info_list(ngx_conf_t *cf, log_shared_info_head_t *h)
{
    if (h != NULL && h->inited == 0) {
        h->pool       = cf->pool;
        h->nodes_size = 0;
        h->begin      = NULL;
        h->end        = NULL;
        h->inited     = 1;
        h->pid        = 0;

        return h;
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// Thread function: flush until the list is empty, then exit
// -----------------------------------------------------------------------------
#if (NGX_THREADS)
static void *
log_thread_func(void *data)
{
    log_shared_info_head_t *head = (log_shared_info_head_t *)data;

    // Keep flushing every 1 second as long as we have items
    for (;;) {
        // If no items left, we can exit the thread
#if (NGX_THREADS)
        pthread_mutex_lock(&mutex);
#endif
        int empty = (head->begin == NULL);
#if (NGX_THREADS)
        pthread_mutex_unlock(&mutex);
#endif

        if (empty) {
            break;
        }

        ngx_log_fflush(head);
        sleep(1);
    }

    // Mark thread ID 0, so a new thread can be started next time we log
    head->pid        = 0;
    head->nodes_size = 0; // or keep it if you prefer

    return NULL;
}
#endif // NGX_THREADS

// -----------------------------------------------------------------------------
// Add a node to the linked list
// -----------------------------------------------------------------------------
static log_shared_info_head_t *
add_info_list(log_shared_info_head_t *head, log_shared_info_t *node)
{
    if (head == NULL || head->inited != 1 || node == NULL) {
        return NULL;
    }

#if (NGX_THREADS)
    pthread_mutex_lock(&mutex);
#endif

    // Allocate a new node on the pool
    log_shared_info_t *p = ngx_palloc(head->pool, sizeof(log_shared_info_t));
    if (p == NULL) {
#if (NGX_THREADS)
        pthread_mutex_unlock(&mutex);
#endif
        return NULL;
    }

    ngx_memcpy(p, node, sizeof(log_shared_info_t));
    p->next = NULL;

    if (head->begin == NULL) {
        // first node in list
        head->begin = p;
        head->end   = p;
    } else {
        // append to end
        head->end->next = p;
        head->end       = p;
    }

    head->nodes_size++;

#if (NGX_THREADS)
    pthread_mutex_unlock(&mutex);
#endif

#if (NGX_THREADS)
    // Start thread if not already started
    if (head->pid == 0) {
        pthread_create(&head->pid, NULL, log_thread_func, head);
    }
#endif

    return head;
}

// -----------------------------------------------------------------------------
// Delete a node from the linked list
// -----------------------------------------------------------------------------
static log_shared_info_head_t *
del_info_list(log_shared_info_head_t *head, log_shared_info_t *node)
{
    if (head == NULL || head->inited != 1 || node == NULL) {
        return head;
    }

    // The list is empty
    if (head->begin == NULL) {
        return head;
    }

    // If the first node is the one to delete
    if (head->begin == node) {
        log_shared_info_t *delete_node = head->begin;
        head->begin = delete_node->next;
        head->nodes_size--;

        // If we removed the last node
        if (head->begin == NULL) {
            head->end = NULL;
        }
        ngx_pfree(head->pool, delete_node);
        return head;
    }

    // Otherwise, find node in the list
    log_shared_info_t *current_node = head->begin;
    while (current_node && current_node->next != node) {
        current_node = current_node->next;
    }
    if (current_node == NULL) {
        // not found
        return head;
    }

    // Remove node->next from the chain
    log_shared_info_t *delete_node = current_node->next;
    current_node->next = delete_node->next;
    head->nodes_size--;

    if (delete_node == head->end) {
        // If we removed the last node
        head->end = current_node;
    }
    ngx_pfree(head->pool, delete_node);

    return head;
}

// -----------------------------------------------------------------------------
// The main handler: gather data, build line, push to the linked list
// -----------------------------------------------------------------------------
static ngx_int_t
ngx_http_custom_log_handler(ngx_http_request_t *r)
{
    ngx_http_custom_log_conf_t *llcf;
    llcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_log_module);
    if (llcf == NULL || llcf->type.len == 0 || r->pool == NULL) {
        return NGX_DECLINED;
    }

    // Gather variables
    ngx_str_t time_local        = ngx_string("time_local");
    ngx_str_t request_id        = ngx_string("request_id");
    ngx_str_t username          = ngx_string("username");
    ngx_str_t sub               = ngx_string("sub");
    ngx_str_t request_platform  = ngx_string("request_platform");
    ngx_str_t geoip_cc          = ngx_string("geoip2_data_country_code");
    ngx_str_t upstream_cache    = ngx_string("upstream_cache_status");
    ngx_str_t challenge_type    = ngx_string("challenge_type");

    ngx_str_t time_local_value       = ngx_null_string;
    ngx_str_t request_id_value       = ngx_null_string;
    ngx_str_t username_value         = ngx_null_string;
    ngx_str_t sub_value             = ngx_null_string;
    ngx_str_t request_platform_value = ngx_null_string;
    ngx_str_t geoip_cc_value        = ngx_null_string;
    ngx_str_t upstream_cache_value   = ngx_null_string;
    ngx_str_t challenge_type_value   = ngx_null_string;

    (void) ngx_http_custom_log_get_variable(r, &time_local,       &time_local_value);
    (void) ngx_http_custom_log_get_variable(r, &request_id,       &request_id_value);
    (void) ngx_http_custom_log_get_variable(r, &username,         &username_value);
    (void) ngx_http_custom_log_get_variable(r, &sub,              &sub_value);
    (void) ngx_http_custom_log_get_variable(r, &request_platform, &request_platform_value);
    (void) ngx_http_custom_log_get_variable(r, &geoip_cc,         &geoip_cc_value);
    (void) ngx_http_custom_log_get_variable(r, &upstream_cache,   &upstream_cache_value);
    (void) ngx_http_custom_log_get_variable(r, &challenge_type,   &challenge_type_value);

    // Build the log line
    u_char log_line[NGX_MAX_ERROR_STR];
    ngx_memzero(log_line, NGX_MAX_ERROR_STR);

    // Trim and sanitize referer, user-agent, etc.
    ngx_str_t referer_value    = (r->headers_in.referer)    ? r->headers_in.referer->value    : ngx_null_string;
    ngx_str_t user_agent_value = (r->headers_in.user_agent) ? r->headers_in.user_agent->value : ngx_null_string;

    if (referer_value.len > HEADER_REFRERER_LEN_LIMIT) {
        referer_value.len = HEADER_REFRERER_LEN_LIMIT;
        referer_value.data[HEADER_REFRERER_LEN_LIMIT] = '\0';
    }
    if (user_agent_value.len > HEADER_USER_AGENT_LEN_LIMIT) {
        user_agent_value.len = HEADER_USER_AGENT_LEN_LIMIT;
        user_agent_value.data[HEADER_USER_AGENT_LEN_LIMIT] = '\0';
    }

    // sanitize them
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

    if (r->unparsed_uri.len > HEADER_UNPARSED_URI_LEN_LIMIT) {
        r->unparsed_uri.len = HEADER_UNPARSED_URI_LEN_LIMIT;
        r->unparsed_uri.data[HEADER_UNPARSED_URI_LEN_LIMIT] = '\0';
    }
    if (r->method_name.len > METHOD_NAME_LEN_LIMIT) {
        r->method_name.len = METHOD_NAME_LEN_LIMIT;
        r->method_name.data[METHOD_NAME_LEN_LIMIT] = '\0';
    }

    // sanitize method
    fmt = FormattingCorrectionStrings(r->pool, r->method_name.data, r->method_name.len);
    if (fmt) {
        r->method_name.data = fmt;
        r->method_name.len  = ngx_strlen(fmt);
    }

    // sanitize custom_data
    if (llcf->custom_data.len > OTHER_CUSTOM_DATA_LEN_LIMIT) {
        llcf->custom_data.len = OTHER_CUSTOM_DATA_LEN_LIMIT;
        llcf->custom_data.data[OTHER_CUSTOM_DATA_LEN_LIMIT] = '\0';
    }
    fmt = FormattingCorrectionStrings(r->pool, llcf->custom_data.data, llcf->custom_data.len);
    if (fmt) {
        llcf->custom_data.data = fmt;
        llcf->custom_data.len  = ngx_strlen(fmt);
    }

    // Possibly wrap "custom_data"
    if (llcf->custom_data.len > 0) {
        static u_char custom_tmp[OTHER_CUSTOM_DATA_LEN_LIMIT+4];
        ngx_memzero(custom_tmp, sizeof(custom_tmp));
        ngx_sprintf(custom_tmp, "%V\"-\"", &llcf->custom_data);
        llcf->custom_data.len  = ngx_strlen(custom_tmp);
        llcf->custom_data.data = custom_tmp;
    }

    // Build the line
    u_char *line_end = NULL;
    if (ngx_strcmp(llcf->type.data, "challenge") == 0) {
        line_end = ngx_slprintf(log_line, log_line + NGX_MAX_ERROR_STR - 1,
            "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"\n",
            &time_local_value,
            &request_id_value,
            &r->connection->addr_text,
            &r->method_name,
            &r->unparsed_uri,
            &challenge_type_value,
            r->headers_out.status,
            &referer_value,
            &request_platform_value,
            &geoip_cc_value,
            &llcf->custom_data,
            &user_agent_value
        );
    }
    else if (ngx_strcmp(llcf->type.data, "waf") == 0) {
        line_end = ngx_slprintf(log_line, log_line + NGX_MAX_ERROR_STR - 1,
            "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%O\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"\n",
            &time_local_value,
            &request_id_value,
            &r->connection->addr_text,
            &r->method_name,
            &r->unparsed_uri,
            r->headers_out.status,
            r->connection->sent,
            &referer_value,
            &request_platform_value,
            &geoip_cc_value,
            &llcf->custom_data,
            &user_agent_value
        );
    }
    else {
        // fallback
        line_end = ngx_slprintf(log_line, log_line + NGX_MAX_ERROR_STR - 1,
            "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%O\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"\n",
            &time_local_value,
            &request_id_value,
            &r->connection->addr_text,
            &r->method_name,
            &r->unparsed_uri,
            r->headers_out.status,
            r->connection->sent,
            &referer_value,
            &request_platform_value,
            &geoip_cc_value,
            &upstream_cache_value,
            &llcf->custom_data,
            &user_agent_value
        );
    }

    // Build the filename
    ngx_str_t log_filename;
    ngx_str_t log_folder;
    ngx_str_t log_format;

    if (username_value.len > 0) {
        ngx_str_set(&log_folder, "/proginter/config/users/");

        if (sub_value.len > 0) {
            ngx_str_set(&log_format, "/domain/subs/%V/logs/last/%V.log");
        } else {
            ngx_str_set(&log_format, "/logs/last/%V.log");
        }
    } else {
        ngx_str_set(&log_folder, "/var/log/nginx/");
        ngx_str_set(&log_format, "/%V.log");
    }

    // Allocate enough for folder + user + sub + type + ".log"
    size_t fname_size = log_folder.len + username_value.len + log_format.len
                      + llcf->type.len + sub_value.len + 32; // safety margin
    log_filename.data = ngx_pnalloc(r->pool, fname_size);
    if (log_filename.data == NULL) {
        return NGX_ERROR;
    }
    ngx_memzero(log_filename.data, fname_size);

    ngx_memcpy(log_filename.data, log_folder.data, log_folder.len);
    if (username_value.len > 0) {
        ngx_memcpy(log_filename.data + log_folder.len,
                   username_value.data, username_value.len);
    }

    if (sub_value.len > 0) {
        ngx_sprintf(log_filename.data + log_folder.len + username_value.len,
                    (char *) log_format.data, &sub_value, &llcf->type);
    } else {
        ngx_sprintf(log_filename.data + log_folder.len + username_value.len,
                    (char *) log_format.data, &llcf->type);
    }
    log_filename.len = ngx_strlen(log_filename.data);

    // Build the node to add
    log_shared_info_t node;
    ngx_memzero(&node, sizeof(node));

    size_t copy_len = (line_end - log_line);
    if (copy_len > sizeof(node.data)) {
        copy_len = sizeof(node.data);
    }
    ngx_memcpy(node.data, log_line, copy_len);
    node.datasize = copy_len;

    size_t fname_cpy_len = (log_filename.len < sizeof(node.log_filename))
                           ? log_filename.len
                           : (sizeof(node.log_filename) - 1);
    ngx_memcpy(node.log_filename, log_filename.data, fname_cpy_len);
    node.log_filename[fname_cpy_len] = '\0';

    // Add to the list
    if (add_info_list(head, &node) == NULL) {
        return NGX_ERROR;
    }

    // If set via Lua, free the strings
    if (llcf->lua) {
        ngx_pfree(r->pool, llcf->type.data);
        ngx_pfree(r->pool, llcf->custom_data.data);
        llcf->type.data        = NULL;
        llcf->type.len         = 0;
        llcf->custom_data.data = NULL;
        llcf->custom_data.len  = 0;
        llcf->lua              = 0;
    }

#if !(NGX_THREADS)
    // If not using threads, flush synchronously every request
    ngx_log_fflush(head);
#endif

    return NGX_OK;
}

// -----------------------------------------------------------------------------
// Helper: remove unwanted chars
// -----------------------------------------------------------------------------
static unsigned char *
FormattingCorrectionStrings(ngx_pool_t *pool, unsigned char *raw, size_t length)
{
    if (!raw || length == 0) {
        return NULL;
    }

    unsigned char *new_str = ngx_pcalloc(pool, length + 1);
    if (new_str == NULL) {
        return NULL;
    }

    size_t line_length = 0;
    for (size_t i = 0; i < length; i++) {
        if (raw[i] != '\n' && raw[i] != '\t' &&
            raw[i] != '\r' && raw[i] != '\"')
        {
            new_str[line_length++] = raw[i];
        }
    }
    new_str[line_length] = '\0';
    return new_str;
}

// -----------------------------------------------------------------------------
// Create + Merge Loc Conf
// -----------------------------------------------------------------------------
static void *
ngx_http_custom_log_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_custom_log_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }
    conf->type.len         = 0;
    conf->type.data        = NULL;
    conf->custom_data.len  = 0;
    conf->custom_data.data = NULL;
    conf->lua              = 0;
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
// Module Init: Register handler + init head + possibly set up Lua preload
// -----------------------------------------------------------------------------
static ngx_int_t
ngx_http_custom_log_init(ngx_conf_t *cf)
{
    // Register the custom log handler in the LOG phase
    ngx_http_core_main_conf_t *cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    if (cmcf == NULL) {
        return NGX_ERROR;
    }

    ngx_http_handler_pt *h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }
    *h = ngx_http_custom_log_handler;

    // Allocate the global head structure
    if (head != NULL) {
        head = NULL; // reset
    }

    head = ngx_palloc(cf->pool, sizeof(log_shared_info_head_t));
    if (head == NULL) {
        return NGX_ERROR;
    }
    ngx_memzero(head, sizeof(*head));

    // Initialize it
    if (init_info_list(cf, head) == NULL) {
        return NGX_ERROR;
    }

#ifdef LOG_IF_LUA_SUPPORT
    // Register the Lua function: require "ngx.custom_log"
    if (ngx_http_lua_add_package_preload(cf, "ngx.custom_log",
                                         ngx_http_lua_custom_log_register_function) != NGX_OK)
    {
        return NGX_ERROR;
    }
#endif

    return NGX_OK;
}

// -----------------------------------------------------------------------------
// Directive 'log_type' handler
// -----------------------------------------------------------------------------
static char *
ngx_http_custom_log_set_type(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_custom_log_conf_t *llcf = conf;
    ngx_str_t *value = cf->args->elts;

    if (llcf == NULL) {
        return NGX_CONF_ERROR;
    }

    // Validate the first argument
    if (strcmp((char *)value[1].data, "dynamic") != 0 &&
        strcmp((char *)value[1].data, "static")  != 0 &&
        strcmp((char *)value[1].data, "ajax")    != 0 &&
        strcmp((char *)value[1].data, "waf")     != 0 &&
        strcmp((char *)value[1].data, "challenge") != 0)
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
// Helper: Get an Nginx variable by name
// -----------------------------------------------------------------------------
static ngx_int_t
ngx_http_custom_log_get_variable(ngx_http_request_t *r, ngx_str_t *name, ngx_str_t *value)
{
    if (r == NULL || name == NULL) {
        return NGX_ERROR;
    }

    ngx_uint_t key = ngx_hash_key(name->data, name->len);
    if (key == (ngx_uint_t)NGX_ERROR) {
        return NGX_ERROR;
    }

    ngx_http_variable_value_t *v = ngx_http_get_variable(r, name, key);
    if (v == NULL || v->not_found || v->data == NULL) {
        return NGX_ERROR;
    }

    value->len  = v->len;
    value->data = v->data;
    return NGX_OK;
}

// -----------------------------------------------------------------------------
// Optional Lua Support
// -----------------------------------------------------------------------------
#ifdef LOG_IF_LUA_SUPPORT

static int
ngx_http_lua_custom_log_register_function(lua_State * L)
{
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, ngx_http_lua_custom_log_set_type);
    lua_setfield(L, -2, "log_type");
    return 1;
}

static int
ngx_http_lua_custom_log_set_type(lua_State * L)
{
    if (lua_gettop(L) != 2) {
        return luaL_error(L, "exactly 2 arguments expected");
    }
    const char *type  = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);

    ngx_http_request_t *r = ngx_http_lua_get_request(L);
    if (r == NULL) {
        return luaL_error(L, "no request object found");
    }

    ngx_http_custom_log_conf_t *llcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_log_module);
    if (llcf == NULL) {
        return 0;
    }

    llcf->type.data = ngx_pnalloc(r->pool, strlen(type)+1);
    if (!llcf->type.data) {
        return luaL_error(L, "no memory for type");
    }
    ngx_memcpy(llcf->type.data, type, strlen(type));
    llcf->type.data[strlen(type)] = '\0';
    llcf->type.len = strlen(type);

    llcf->custom_data.data = ngx_pnalloc(r->pool, strlen(value)+1);
    if (!llcf->custom_data.data) {
        return luaL_error(L, "no memory for custom_data");
    }
    ngx_memcpy(llcf->custom_data.data, value, strlen(value));
    llcf->custom_data.data[strlen(value)] = '\0';
    llcf->custom_data.len = strlen(value);

    llcf->lua = 1;
    lua_pushboolean(L, 1);
    return 1;
}

#endif // LOG_IF_LUA_SUPPORT
