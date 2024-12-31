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

#define MAX_LOG_LINE_LEN 4096
#define HEADER_USER_AGENT_LEN_LIMIT 350
#define HEADER_UNPARSED_URI_LEN_LIMIT 350
#define METHOD_NAME_LEN_LIMIT 20
#define OTHER_CUSTOM_DATA_LEN_LIMIT 100
#define HEADER_REFRERER_LEN_LIMIT 100
#define MAX_LIST_BUFFER_SIZE 100
#define LIST_BUFFER_FLUSH_DEPLAY_MS 1000

typedef struct {
    ngx_str_t type;
    ngx_str_t custom_data;
    int lua;
} ngx_http_custom_log_conf_t;

typedef struct {
    ngx_hash_t log_lines_hash;
    ngx_hash_keys_arrays_t  log_lines_hash_keys;
} ngx_http_custom_log_main_conf_t;

typedef struct {
    ngx_http_request_t *request;
    ngx_str_t type;
} ngx_http_custom_log_ctx_t;

typedef struct {
    ngx_buf_t *buf;
    size_t size;
} log_lines_buffer_t;

typedef struct log_shared_info {
    unsigned char data[NGX_MAX_ERROR_STR];
    unsigned char log_filename[256];
    size_t datasize;
    struct log_shared_info * volatile next;
} log_shared_info_t;

typedef struct {
    int inited;
    log_shared_info_t * begin;
    log_shared_info_t * end;
    ngx_pool_t  *pool;
    pthread_t pid;
    size_t nodes_size;
} log_shared_info_head_t;

#if (NGX_THREADS)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static log_shared_info_head_t *head = NULL;

static char *ngx_http_custom_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static void *ngx_http_custom_log_create_loc_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_custom_log_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_custom_log_init(ngx_conf_t *cf);
static char *ngx_http_custom_log_set_type(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_custom_log_get_variable(ngx_http_request_t *r, ngx_str_t *name, ngx_str_t *value);
static void ngx_log_fflush(log_shared_info_head_t *head);
static log_shared_info_head_t *init_info_list(ngx_conf_t *cf, log_shared_info_head_t *head);
static log_shared_info_head_t *add_info_list(log_shared_info_head_t *head, log_shared_info_t *node);
static log_shared_info_head_t *del_info_list(log_shared_info_head_t *head, log_shared_info_t *node);
static unsigned char *FormattingCorrectionStrings(ngx_pool_t *pool, unsigned char *raw, size_t length);
#ifdef LOG_IF_LUA_SUPPORT
static int ngx_http_lua_custom_log_set_type(lua_State * L);
static int ngx_http_lua_custom_log_register_function(lua_State * L);
#endif

#if (NGX_THREADS)
static void *log_thread_func(void *data);
#endif

static ngx_command_t ngx_http_custom_log_commands[] = {
    { ngx_string("log_type"),
    NGX_HTTP_LOC_CONF | NGX_CONF_TAKE2,
    ngx_http_custom_log_set_type,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL },
    ngx_null_command
};

static ngx_http_module_t ngx_http_custom_log_module_ctx = {
    NULL, /* preconfiguration */
    ngx_http_custom_log_init, /* postconfiguration */

    NULL,  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_custom_log_create_loc_conf,   /* create location configuration */
    ngx_http_custom_log_merge_loc_conf     /* merge location configuration */
};

ngx_module_t ngx_http_custom_log_module = {
    NGX_MODULE_V1,
    &ngx_http_custom_log_module_ctx, /* module context */
    ngx_http_custom_log_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    NULL, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};

static void ngx_log_fflush(log_shared_info_head_t *head) {
    if(head == NULL || head->inited == 0 ) {
        return;
    }

    static ngx_msec_t prevcall_time = 0;

    if(head->nodes_size < MAX_LIST_BUFFER_SIZE) {
        if(prevcall_time == 0) {
            prevcall_time = ngx_time();
        } else {
            ngx_msec_t currentcall_time = ngx_time();
            ngx_msec_t difftime = (currentcall_time - prevcall_time) * 1000;
            if(difftime < LIST_BUFFER_FLUSH_DEPLAY_MS) {
                return;
            }
            prevcall_time = currentcall_time;
        }
    } else {
        //reset timer
        prevcall_time = ngx_time();
    }



#if (NGX_THREADS)
    pthread_mutex_lock(&mutex);
#endif

    log_shared_info_t *info = (log_shared_info_t*)head->begin;

#if (NGX_THREADS)
    pthread_mutex_unlock(&mutex);
#endif

    int while_begin = 0;

    while(info != NULL) {
        while_begin = 1;
#if (NGX_THREADS)
        pthread_mutex_lock(&mutex);
#endif
        unsigned char *line_buffer = info->data;

#if (NGX_THREADS)
        pthread_mutex_unlock(&mutex);
#endif

        ngx_fd_t fd = ngx_open_file(info->log_filename, NGX_FILE_APPEND, NGX_FILE_CREATE_OR_OPEN, NGX_FILE_DEFAULT_ACCESS);

        if(fd == NGX_INVALID_FILE) {
            info = info->next;
            while_begin = 0;
            continue;
        }

        ngx_write_fd(fd, line_buffer, info->datasize);

#if (NGX_THREADS)
        pthread_mutex_lock(&mutex);
#endif

        log_shared_info_t *delete_node = info;

        info = info->next;

#if (NGX_THREADS)
        pthread_mutex_unlock(&mutex);
#endif

        ngx_close_file(fd);
        del_info_list(head,delete_node);
        
    }

    if(while_begin) {
#if (NGX_THREADS)
        pthread_mutex_unlock(&mutex);
#endif
    }
}


static log_shared_info_head_t *init_info_list(ngx_conf_t *cf, log_shared_info_head_t *head) {
    if(head != NULL && head->inited == 0) {
        head->pool = cf->pool;
        head->nodes_size = 0;
        head->begin = NULL;
        head->end = NULL;
        head->inited = 1;
        head->pid = 0;
        return head;

    } else {
        return NULL;
    }
}

#if (NGX_THREADS)
static void *log_thread_func(void *data) {
    log_shared_info_head_t *head = (log_shared_info_head_t*)data;
    while(head->begin != NULL) {
        ngx_log_fflush(head);
	sleep(1);
    }
    head->pid = 0;
    head->nodes_size = 0;
    return NULL;
}
#endif

static log_shared_info_head_t *add_info_list(log_shared_info_head_t *head, log_shared_info_t *node) {
    if(head != NULL && head->inited == 1 && node != NULL) {
        log_shared_info_t *current_node = head->end;

        if(current_node == NULL) {
            log_shared_info_t *p = ngx_palloc(head->pool, sizeof(log_shared_info_t));
#if (NGX_THREADS)
            pthread_mutex_lock(&mutex);
#endif
            head->begin = p;
            head->end = head->begin;
            head->nodes_size++;
#if (NGX_THREADS)
            pthread_mutex_unlock(&mutex);
#endif
            if(head->begin == NULL) {
                return NULL;
            }

#if (NGX_THREADS)
            pthread_mutex_lock(&mutex);
#endif

            ngx_memcpy(head->begin, node, sizeof(log_shared_info_t));

#if (NGX_THREADS)
            pthread_mutex_unlock(&mutex);
#endif

#if (NGX_THREADS)
            if(head->pid == 0) {
                pthread_create(&head->pid, NULL, log_thread_func, head);
            }
#endif

        } else {
            log_shared_info_t *p = ngx_palloc(head->pool, sizeof(log_shared_info_t));
            
#if (NGX_THREADS)
            pthread_mutex_lock(&mutex);
#endif
            current_node->next = p;
            head->nodes_size++;
            head->end=current_node->next;
#if (NGX_THREADS)
            pthread_mutex_unlock(&mutex);
#endif
            if(current_node->next == NULL) {
                return NULL;
            }

#if (NGX_THREADS)
            pthread_mutex_lock(&mutex);
#endif

            ngx_memcpy(current_node->next, node, sizeof(log_shared_info_t));
#if (NGX_THREADS)
            pthread_mutex_unlock(&mutex);
#endif

#if (NGX_THREADS)
            if(head->pid == 0) {
                pthread_create(&head->pid, NULL, log_thread_func, head);
            }
#endif
        }

        return head;
        
    } else {
        return NULL;
    }
}

static log_shared_info_head_t *del_info_list(log_shared_info_head_t *head, log_shared_info_t *node) {
    if(head != NULL && head->inited == 1 && node != NULL) {

        log_shared_info_t *current_node = head->begin;
        if(current_node == NULL) {
            return head;
        } else if (head->begin == node) {
            log_shared_info_t *delete_node = head->begin;
#if (NGX_THREADS)
            pthread_mutex_lock(&mutex);
#endif
            head->end = delete_node->next;
            head->begin = delete_node->next;
            head->nodes_size--;
#if (NGX_THREADS)
            pthread_mutex_unlock(&mutex);
#endif
            ngx_pfree(head->pool, delete_node);
        }else {
            while(current_node != NULL && current_node != node) {
                current_node = current_node->next;
            }

	    if(current_node == NULL) {
	    	return NULL;
	    }

            log_shared_info_t *delete_node = current_node->next;
#if (NGX_THREADS)
            pthread_mutex_lock(&mutex);
#endif
            current_node->next = delete_node->next;
            head->nodes_size--;
            if(delete_node->next == NULL) {
                head->end = current_node;
            }
#if (NGX_THREADS)
            pthread_mutex_unlock(&mutex);
#endif
            ngx_pfree(head->pool, delete_node);
        }
    } 
    return NULL;
}

static ngx_int_t ngx_http_custom_log_handler(ngx_http_request_t *r) {
    ngx_http_custom_log_conf_t *llcf;
    ngx_http_custom_log_ctx_t *ctx;
    ngx_str_t type;

    /* Get location configuration */
    llcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_log_module);

    /* Check if type are set */
    if (llcf == NULL || llcf->type.len == 0 || r->pool == NULL) {
        return NGX_DECLINED;
    }

    /* Save the context for the request */
    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_custom_log_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ctx->request = r;
    ctx->type = llcf->type;

    type = llcf->type;

    /* Get the time_local */
    ngx_str_t time_local;
    ngx_str_set(&time_local, "time_local");

    ngx_str_t time_local_value;
    ngx_str_set(&time_local_value, "time_local");
    if (ngx_http_custom_log_get_variable(r, &time_local, &time_local_value) != NGX_OK) {
        time_local_value.len = 0;
    }

    /* Get the request_id */
    ngx_str_t request_id;
    ngx_str_set(&request_id, "request_id");

    ngx_str_t request_id_value;
    ngx_str_set(&request_id_value, "request_id");
    if (ngx_http_custom_log_get_variable(r, &request_id, &request_id_value) != NGX_OK) {
        request_id_value.len = 0;
    }

    /* Get the username */
    ngx_str_t username;
    ngx_str_set(&username, "username");

    ngx_str_t username_value;
    ngx_str_set(&username_value, "username");
    if (ngx_http_custom_log_get_variable(r, &username, &username_value) != NGX_OK) {
        username_value.len = 0;
    }

    /* Get the sub */
    ngx_str_t sub;
    ngx_str_set(&sub, "sub");

    ngx_str_t sub_value;
    ngx_str_set(&sub_value, "sub");
    if (ngx_http_custom_log_get_variable(r, &sub, &sub_value) != NGX_OK) {
        sub_value.len = 0;
    }

    /* Get the request_platform */
    ngx_str_t request_platform;
    ngx_str_set(&request_platform, "request_platform");

    ngx_str_t request_platform_value;
    ngx_str_set(&request_platform_value, "request_platform");
    if (ngx_http_custom_log_get_variable(r, &request_platform, &request_platform_value) != NGX_OK) {
        request_platform_value.len = 0;
    }

    /* Get the geoip2_data_country_code */
    ngx_str_t geoip2_data_country_code;
    ngx_str_set(&geoip2_data_country_code, "geoip2_data_country_code");

    ngx_str_t geoip2_data_country_code_value;
    ngx_str_set(&geoip2_data_country_code_value, "geoip2_data_country_code");
    if (ngx_http_custom_log_get_variable(r, &geoip2_data_country_code, &geoip2_data_country_code_value) != NGX_OK) {
        geoip2_data_country_code_value.len = 0;
    }

    /* Get the upstream_cache_status */
    ngx_str_t upstream_cache_status;
    ngx_str_set(&upstream_cache_status, "upstream_cache_status");

    ngx_str_t upstream_cache_status_value;
    ngx_str_set(&upstream_cache_status_value, "upstream_cache_status");
    if (ngx_http_custom_log_get_variable(r, &upstream_cache_status, &upstream_cache_status_value) != NGX_OK) {
        upstream_cache_status_value.len = 0;
    }

    /* Get the challenge_type */
    ngx_str_t challenge_type;
    ngx_str_set(&challenge_type, "challenge_type");

    ngx_str_t challenge_type_value;
    ngx_str_set(&challenge_type_value, "challenge_type");
    if (ngx_http_custom_log_get_variable(r, &challenge_type, &challenge_type_value) != NGX_OK) {
        challenge_type_value.len = 0;
    }
    
    u_char *line_end;

    u_char log_line[NGX_MAX_ERROR_STR]={};

    ngx_str_t referer_value = ngx_null_string;
    ngx_str_t user_agent_value = ngx_null_string;

    if (r->headers_in.referer != NULL) {
        referer_value = r->headers_in.referer->value;
    }

    if (r->headers_in.user_agent != NULL) {
        user_agent_value = r->headers_in.user_agent->value;
    }

    unsigned char *formatstring = NULL;

    if(referer_value.len > HEADER_REFRERER_LEN_LIMIT) {
        referer_value.data[HEADER_REFRERER_LEN_LIMIT] = '\0';
        referer_value.len = HEADER_REFRERER_LEN_LIMIT;
    }

    formatstring = NULL;
    formatstring = FormattingCorrectionStrings(r->pool, referer_value.data, referer_value.len);
    if(formatstring != NULL) {
        referer_value.data = formatstring;
        referer_value.len = strlen((char *)formatstring);
    }

    if(user_agent_value.len > HEADER_USER_AGENT_LEN_LIMIT) {
        user_agent_value.data[HEADER_USER_AGENT_LEN_LIMIT] = '\0';
        user_agent_value.len = HEADER_USER_AGENT_LEN_LIMIT;
    }

    formatstring = NULL;
    formatstring = FormattingCorrectionStrings(r->pool, user_agent_value.data, user_agent_value.len);
    if(formatstring != NULL) {
        user_agent_value.data = formatstring;
        user_agent_value.len = strlen((char *)formatstring);
    }

    if(r->unparsed_uri.len > HEADER_UNPARSED_URI_LEN_LIMIT) {
        r->unparsed_uri.data[HEADER_UNPARSED_URI_LEN_LIMIT] = '\0';
        r->unparsed_uri.len = HEADER_UNPARSED_URI_LEN_LIMIT;
    }

    if(r->method_name.len > METHOD_NAME_LEN_LIMIT) {
        r->method_name.data[METHOD_NAME_LEN_LIMIT] = '\0';
        r->method_name.len = METHOD_NAME_LEN_LIMIT;
    }

    formatstring = NULL;
    formatstring = FormattingCorrectionStrings(r->pool, r->method_name.data, r->method_name.len);
    if(formatstring != NULL) {
        r->method_name.data = formatstring;
        r->method_name.len = strlen((char *)formatstring);
    }

    if(llcf->custom_data.len > OTHER_CUSTOM_DATA_LEN_LIMIT) {
        llcf->custom_data.data[OTHER_CUSTOM_DATA_LEN_LIMIT] = '\0';
        llcf->custom_data.len = OTHER_CUSTOM_DATA_LEN_LIMIT;
    }

    formatstring = NULL;
    formatstring = FormattingCorrectionStrings(r->pool, llcf->custom_data.data, llcf->custom_data.len);
    if(formatstring != NULL) {
        llcf->custom_data.data = formatstring;
        llcf->custom_data.len = strlen((char *)formatstring);
    }

    unsigned char custom_tmp[OTHER_CUSTOM_DATA_LEN_LIMIT+1] = "";

    if (llcf->custom_data.len > 0) {
        ngx_sprintf(custom_tmp, "%V\"-\"", &llcf->custom_data);
        llcf->custom_data.len = ngx_strlen(custom_tmp);
        llcf->custom_data.data = custom_tmp;
    }

    if (strcmp((const char *)type.data, "challenge") == 0) {
        line_end = ngx_slprintf(log_line, log_line + NGX_MAX_ERROR_STR - 1,
                                "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"" CRLF,
                                &time_local_value,
                                &request_id_value,
                                &r->connection->addr_text,
                                &r->method_name,
                                &r->unparsed_uri,
                                &challenge_type_value,
                                r->headers_out.status,
                                &referer_value,
                                &request_platform_value,
                                &geoip2_data_country_code_value,
                                &llcf->custom_data,
                                &user_agent_value);
    }
    else if (strcmp((const char *)type.data, "waf") == 0) {
        line_end = ngx_slprintf(log_line, log_line + NGX_MAX_ERROR_STR - 1,
                                "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%O\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"" CRLF,
                                &time_local_value,
                                &request_id_value,
                                &r->connection->addr_text,
                                &r->method_name,
                                &r->unparsed_uri,
                                r->headers_out.status,
                                r->connection->sent,
                                &referer_value,
                                &request_platform_value,
                                &geoip2_data_country_code_value,
                                &llcf->custom_data,
                                &user_agent_value);
    }
    else {
        line_end = ngx_slprintf(log_line, log_line + NGX_MAX_ERROR_STR - 1,
                                "\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%ui\"-\"%O\"-\"%V\"-\"%V\"-\"%V\"-\"%V\"-\"%V%V\"" CRLF,
                                &time_local_value,
                                &request_id_value,
                                &r->connection->addr_text,
                                &r->method_name,
                                &r->unparsed_uri,
                                r->headers_out.status,
                                r->connection->sent,
                                &referer_value,
                                &request_platform_value,
                                &geoip2_data_country_code_value,
                                &upstream_cache_status_value,
                                &llcf->custom_data,
                                &user_agent_value);
    }

    ngx_str_t log_filename;
    ngx_str_t log_folder;
    ngx_str_t log_format;

    if (username_value.len > 0) {
        ngx_str_set(&log_folder, "/proginter/config/users/");

        if (sub_value.len > 0) {
            ngx_str_set(&log_format, "/domain/subs/%V/logs/last/%V.log");
        }
        else {
            ngx_str_set(&log_format, "/logs/last/%V.log");
        }
    } else {
        ngx_str_set(&log_folder, "/var/log/nginx/");
        ngx_str_set(&log_format, "/%V.log");
    }
    
    log_filename.data = ngx_pnalloc(r->pool, log_folder.len + username_value.len + log_format.len + type.len + sub_value.len);
    ngx_memzero(log_filename.data,log_folder.len + username_value.len + log_format.len + type.len + sub_value.len);
    if (log_filename.data == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(log_filename.data, log_folder.data, log_folder.len);
    if (username_value.len > 0) {
        ngx_memcpy(log_filename.data + log_folder.len, username_value.data, username_value.len);
    }

    if (sub_value.len == 0) {
        ngx_sprintf(log_filename.data + log_folder.len + username_value.len, (char *) log_format.data, &type);
    }
    else {
        ngx_sprintf(log_filename.data + log_folder.len + username_value.len, (char *) log_format.data, &sub_value, &type);
    }
    log_filename.len = ngx_strlen(log_filename.data);

    log_shared_info_t node;
    ngx_memzero(&node, sizeof(log_shared_info_t));

    ngx_memcpy(node.data, log_line, line_end - log_line);
    node.datasize = line_end - log_line;
    ngx_memcpy(node.log_filename, log_filename.data, log_filename.len);

    if(add_info_list(head, &node) == NULL) {
        return NGX_ERROR;
    }

    if(llcf->lua) {
        ngx_pfree(r->pool, llcf->type.data);
        ngx_pfree(r->pool, llcf->custom_data.data);
        llcf->type.data = NULL;
        llcf->type.len = 0;
        llcf->custom_data.data = NULL;
        llcf->custom_data.len = 0;
    }

#if !(NGX_THREADS)
    ngx_log_fflush(&head);
#endif 

    return NGX_OK;
}

static unsigned char *FormattingCorrectionStrings(ngx_pool_t *pool, unsigned char *raw, size_t length) {
    unsigned char *new = ngx_pcalloc(pool, length);
    if(new != NULL) {
        //Delete unwanted characters
        int remove_item = 0;
        int line_length = 0;

        for(int i=0; i < (int)length; i++) {
            if((raw[i] != '\n') &&
                (raw[i] != '\t') &&
                (raw[i] != '\r') &&
                (raw[i] != '\"')) {
                 new[i-remove_item] = raw[i];
                 line_length++;
            } else {
                 remove_item++;
            }
        }
        new[line_length] = '\0';
    }
    return new;
}

static void *ngx_http_custom_log_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_custom_log_conf_t *conf;

    if (cf->pool == NULL) {
        return NULL;
    }

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_custom_log_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->type.len = 0;
    conf->custom_data.len = 0;

    return conf;
}

static char *ngx_http_custom_log_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child) {
    ngx_http_custom_log_conf_t *prev = parent;
    ngx_http_custom_log_conf_t *conf = child;

    if (prev == NULL || conf == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_conf_merge_str_value(conf->type, prev->type, "");
    ngx_conf_merge_str_value(conf->custom_data, prev->custom_data, "");
 
    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_custom_log_init(ngx_conf_t *cf) {
    ngx_http_core_main_conf_t        *cmcf;
    ngx_http_handler_pt              *h;

    // Register the custom log handler
    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    if (cmcf == NULL) {
        return NGX_ERROR;
    }

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_custom_log_handler;

    if(head != NULL) {
        head = NULL;
    }

    head = ngx_palloc(cf->pool, sizeof(log_shared_info_head_t));
    if(head == NULL) {
        return NGX_ERROR;
    }

    head->inited = 0;
    if(head->inited == 0)  {
        if(init_info_list(cf, head) == NULL) {
            return NGX_ERROR;
        }
    }

#ifdef LOG_IF_LUA_SUPPORT
    if (ngx_http_lua_add_package_preload(cf, "ngx.custom_log", ngx_http_lua_custom_log_register_function) != NGX_OK)
    {
        return NGX_ERROR;
    }
#endif


    return NGX_OK;
}

static char *ngx_http_custom_log_set_type(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_custom_log_conf_t *llcf = conf;

    ngx_str_t *value;

    if (llcf == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (strcmp((char *)value[1].data, "dynamic") != 0 &&
        strcmp((char *)value[1].data, "static") != 0 &&
        strcmp((char *)value[1].data, "ajax") != 0 &&
        strcmp((char *)value[1].data, "waf") != 0 &&
        strcmp((char *)value[1].data, "challenge") != 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                        "invalid log_type value \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    llcf->type = value[1];
    llcf->custom_data = value[2];

    llcf->lua = 0;

    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_custom_log_get_variable(ngx_http_request_t *r, ngx_str_t *name, ngx_str_t *value) {
    ngx_http_variable_value_t *v;
    ngx_uint_t key;

    if (r == NULL || name == NULL) {
        return NGX_ERROR;
    }

    key = ngx_hash_key(name->data, name->len);
    if (key == (ngx_uint_t)NGX_ERROR) {
        return NGX_ERROR;
    }

    v = ngx_http_get_variable(r, name, key);

    if (v == NULL || v->not_found || v->data == NULL) {
        return NGX_ERROR;
    }

    value->len = v->len;
    value->data = v->data;

    return NGX_OK;
}

#ifdef LOG_IF_LUA_SUPPORT
static int ngx_http_lua_custom_log_register_function(lua_State * L) {
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, ngx_http_lua_custom_log_set_type);
    lua_setfield(L, -2, "log_type");
    return 1;
}

static int ngx_http_lua_custom_log_set_type(lua_State * L) {
    if (lua_gettop(L) != 2) {
        return luaL_error(L, "exactly 2 arguments expected");
    }
    const char *type = luaL_checkstring(L, 1);
    const char *value = luaL_checkstring(L, 2);

    ngx_http_request_t *r = NULL;

    r = ngx_http_lua_get_request(L);
    if (r == NULL) {
        return luaL_error(L, "no request object found");
    }

    ngx_http_custom_log_conf_t *llcf;

    llcf = ngx_http_get_module_loc_conf(r, ngx_http_custom_log_module);

    if (llcf == NULL) {
        return 0;
    }

    llcf->type.data = ngx_pnalloc(r->pool, strlen(type)+1);
    ngx_memzero(llcf->type.data, strlen(type) + 1);
    ngx_memcpy(llcf->type.data, type, strlen(type));
    llcf->type.len = strlen(type);

    llcf->custom_data.data = ngx_pnalloc(r->pool, strlen(value) + 1);
    ngx_memzero(llcf->custom_data.data, strlen(value) + 1);
    ngx_memcpy(llcf->custom_data.data, value, strlen(value));
    llcf->custom_data.len = strlen(value);

    llcf->lua = 1;

    return 1;

}
#endif
