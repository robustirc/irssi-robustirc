// vim:ts=4:sw=4:et
// © 2015 Michael Stapelberg (see COPYING)

// stdlib includes
#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

// external library includes
#include <curl/curl.h>
#include <gio/gio.h>
#include <glib.h>
#include <yajl/yajl_gen.h>
#include <yajl/yajl_tree.h>
#include <yajl/yajl_parse.h>

// irssi includes
#include "common.h"
#include "misc.h"
#include "network.h"
#include "levels.h"
#include "printtext.h"
#include "irc.h"
#include "irc-servers.h"
#include "rawlog.h"

// module includes
#include "robustirc.h"
#include "module-formats.h"
#include "robustsession-network.h"

// from http://robustirc.net/docs/robustsession.html#getmessages
static const long robustirc_to_client = 3;
static const long robustping = 4;

static CURLM *curl_handle;

// |server_ctx| stores context objects, keyed by an IRC_SERVER_REC pointer.
// Used to attach data to an IRC_SERVER_REC without modifying irssi core code.
static GHashTable *server_ctx;

// TODO: when is this freed?
struct t_robustsession_ctx {
    char *sessionid;
    char *sessionauth;
    char *lastseen;
    struct curl_slist *headers;

    GList *curl_handles;
};

struct t_body_buffer {
    char *body;
    size_t size;
};

// TODO: create a constructor/destructor, figure out what’s idiomatic with glib
struct t_robustirc_request {
    enum {
        RT_CREATESESSION = 0,
        RT_DELETESESSION = 1,
        RT_POSTMESSAGE = 2,
        RT_GETMESSAGES = 3,
    } type;

    char curl_error_buf[CURL_ERROR_SIZE];

    // |target| is the host:port to which this request is currently being sent.
    char *target;

    // Do not free. Used to prolong the GetMessages timeout when receiving a
    // RobustPing message.
    CURL *curl;

    // |url_suffix| contains the part of the URL after the host:port, so that
    // the correct URL can easily be re-assembled with a new |target|.
    char *url_suffix;

    SERVER_REC *server;
    struct t_body_buffer *body;

    // Used when type == RT_GETMESSAGES.
    guint timeout_tag;
    struct t_robustsession_ctx *ctx;
    yajl_handle parser;
    char *last_key;
    char *data;
    bool parsing_id;
    bool parsing_servers;
    long last_id_id;
    long last_id_reply;
    long last_type;
    int depth;
    GList *servers;
};

static void get_messages(const char *target, gpointer userdata);
static gboolean get_messages_timeout(gpointer userdata);
static void curl_set_common_options(CURL *curl,
                                    struct t_robustsession_ctx *ctx,
                                    SERVER_REC *server,
                                    struct t_robustirc_request *request);

// Feeds messages such as the following into the JSON parser:
//
// {"Id":     {"Id":1428773900924989332,"Reply":1},
//  "Session":{"Id":1428773900606543398,"Reply":0},
//  "Type":   3,
//  "Data":   ":robustirc.net 311 sECuRE blorgh michael robust/0x13d4059e24c28428 * :Michael Stapelberg"}
//
// or (a ping message):
//
// {"Id":     {"Id":0,"Reply":0},
//  "Session":{"Id":0,"Reply":0},
//  "Type":   4,
//  "Data":   "",
//  "Servers":["localhost:13003","localhost:13001","localhost:13002"]}
static size_t
gm_write_func(void *ptr, size_t size, size_t nmemb, void *userdata) {
    struct t_robustirc_request *request = userdata;
    // We can safely multiply size * nmemb without overflow checking because
    // curl_easy_setopt(3), section CURLOPT_WRITEFUNCTION specifies that (size
    // * nmemb) < CURL_MAX_WRITE_SIZE == 16 KiB.
    if (yajl_parse(request->parser, ptr, size * nmemb) != yajl_status_ok) {
        unsigned char *yajl_error =
            yajl_get_error(request->parser, 0, ptr, size * nmemb);
        gchar *chunk = g_strdup(ptr);
        gchar *error = g_strdup((const char *)yajl_error);
        g_strstrip(chunk);
        g_strstrip(error);
        printformat_module(MODULE_NAME, request->server, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_PARSE_JSON,
                           chunk, error);
        g_free(chunk);
        g_free(error);
        yajl_free_error(request->parser, yajl_error);
    }
    return size * nmemb;
}

static int gm_json_map_key(void *ctx, const unsigned char *val, size_t len) {
    struct t_robustirc_request *request = ctx;

    free(request->last_key);
    request->last_key = g_new0(char, len + 1);
    memcpy(request->last_key, val, len);

    return 1;
}

static int gm_json_integer(void *ctx, long long val) {
    struct t_robustirc_request *request = ctx;
    if (!request->last_key) {
        return 1;
    }
    if (request->parsing_id) {
        if (strcasecmp(request->last_key, "id") == 0) {
            request->last_id_id = val;
        } else if (strcasecmp(request->last_key, "reply") == 0) {
            request->last_id_reply = val;
        }
    }
    if (strcasecmp(request->last_key, "type") == 0) {
        request->last_type = val;
    }
    return 1;
}

static int gm_json_string(void *ctx, const unsigned char *val, size_t len) {
    struct t_robustirc_request *request = ctx;
    if (request->parsing_servers) {
        char *str = g_new0(char, len + 1);
        memcpy(str, val, len);
        request->servers = g_list_append(request->servers, str);
        return 1;
    }
    if (!request->last_key) {
        return 1;
    }
    if (strcasecmp(request->last_key, "data") == 0) {
        free(request->data);
        request->data = g_new0(char, len + 1);
        memcpy(request->data, val, len);
    }
    return 1;
}

static int gm_json_start_array(void *ctx) {
    struct t_robustirc_request *request = ctx;

    if (request->last_key && strcasecmp(request->last_key, "servers") == 0) {
        request->parsing_servers = true;
        g_list_free_full(request->servers, g_free);
    }
    return 1;
}

static int gm_json_end_array(void *ctx) {
    struct t_robustirc_request *request = ctx;
    request->parsing_servers = false;
    return 1;
}

static int gm_json_start_map(void *ctx) {
    struct t_robustirc_request *request = ctx;
    request->parsing_id =
        (request->last_key && strcasecmp(request->last_key, "id") == 0);
    request->depth++;
    return 1;
}

static int gm_json_end_map(void *ctx) {
    struct t_robustirc_request *request = ctx;
    request->parsing_id = false;
    request->depth--;
    if (request->depth > 0) {
        return 1;
    }
    // TODO: need to confirm the server is connected and has a rawlog, otherwise segfault
    if (request->data != NULL && request->last_type == robustirc_to_client) {
        rawlog_input(request->server->rawlog, request->data);
        signal_emit("server incoming", 2, request->server, request->data);
        free(request->data);
        request->data = NULL;
        free(request->ctx->lastseen);
        request->ctx->lastseen = g_strdup_printf(
            "%ld.%ld",
            request->last_id_id,
            request->last_id_reply);
    }
    if (request->last_type == robustping) {
        g_source_remove(request->timeout_tag);
        request->timeout_tag = g_timeout_add_seconds(
            60, get_messages_timeout, request->curl);
        robustsession_network_update_servers(
            request->server->connrec->address, request->servers);
        request->servers = NULL;
    }

    robustsession_network_succeeded(
        request->server->connrec->address, request->target);

    return 1;
}

static yajl_callbacks gm_callbacks = {
    NULL,
    NULL,
    gm_json_integer,
    NULL,
    NULL,
    gm_json_string,
    gm_json_start_map,
    gm_json_map_key,
    gm_json_end_map,
    gm_json_start_array,
    gm_json_end_array};

static gboolean get_messages_timeout(gpointer userdata) {
    CURL *curl = userdata;
    struct t_robustirc_request *request = NULL;

    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &request);

    SERVER_REC *server = request->server;
    gchar *address = NULL;
    if (request->server->connrec && request->server->connrec->address) {
        address = g_strdup(request->server->connrec->address);
        robustsession_network_failed(address, request->target);
    }

    printtext(NULL, NULL, MSGLEVEL_CRAP, "get_messages_timeout");

    curl_multi_remove_handle(curl_handle, curl);
    request->ctx->curl_handles = g_list_remove(request->ctx->curl_handles, curl);
    curl_easy_cleanup(curl);
    free(request->body->body);
    free(request->body);
    free(request->target);
    free(request);

    if (address) {
        robustsession_network_server(address, TRUE, get_messages, server);
        g_free(address);
    }

    return G_SOURCE_REMOVE;
}

static void get_messages(const char *target, gpointer userdata) {
    SERVER_REC *server = userdata;
    struct t_robustirc_request *request = NULL;
    struct t_robustsession_ctx *ctx = NULL;

    if (!(ctx = g_hash_table_lookup(server_ctx, server))) {
        return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printformat_module(MODULE_NAME, server, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                           "curl_easy_init() failed. Out of memory?");
        return;
    }
    request = g_new0(struct t_robustirc_request, 1);
    request->ctx = ctx;
    request->type = RT_GETMESSAGES;
    request->body = g_new0(struct t_body_buffer, 1);
    request->server = server;
    request->url_suffix = g_strdup_printf("/robustirc/v1/%s/messages",
                                          ctx->sessionid);
    request->target = g_strdup(target);
    request->curl = curl;
    request->timeout_tag = g_timeout_add_seconds(
        60, get_messages_timeout, curl);

    yajl_handle hand = yajl_alloc(&gm_callbacks, NULL, request);
    yajl_config(hand, yajl_allow_multiple_values, 1);
    request->parser = hand;
    gchar *url = g_strdup_printf(
        "https://%s%s?lastseen=%s",
        request->target,
        request->url_suffix,
        ctx->lastseen);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    g_free(url);
    curl_set_common_options(curl, ctx, server, request);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, gm_write_func);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0);

    /* Make libcurl immediately start handling the request. */
    curl_multi_add_handle(curl_handle, curl);
    ctx->curl_handles = g_list_append(ctx->curl_handles, curl);
    int running;
    curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0, &running);
}

static bool create_session_done(struct t_robustirc_request *request, CURL *curl) {
    yajl_val root, sessionid, sessionauth;
    char errmsg[1024];
    const char *ip_address;

    root = yajl_tree_parse((const char *)request->body->body, errmsg, sizeof(errmsg));
    if (root == NULL) {
        gchar *chunk = g_strdup(request->body->body);
        gchar *error = g_strdup(errmsg);
        g_strstrip(chunk);
        g_strstrip(error);
        printformat_module(MODULE_NAME, request->server, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_PARSE_JSON,
                           chunk, error);
        g_free(chunk);
        g_free(error);
        return false;
    }

    if (!(sessionid = yajl_tree_get(root, (const char *[]){"Sessionid", NULL}, yajl_t_string))) {
        printtext(NULL, NULL, MSGLEVEL_CRAP, "sessionid not found");
        yajl_tree_free(root);
        return false;
    }

    if (!(sessionauth = yajl_tree_get(root, (const char *[]){"Sessionauth", NULL}, yajl_t_string))) {
        printtext(NULL, NULL, MSGLEVEL_CRAP, "sessionauth not found");
        yajl_tree_free(root);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ip_address);
    struct t_robustsession_ctx *ctx = request->ctx;
    ctx->sessionid = g_strdup(YAJL_GET_STRING(sessionid));
    ctx->sessionauth = g_strdup(YAJL_GET_STRING(sessionauth));
    ctx->headers = curl_slist_append(ctx->headers, "Accept: application/json");
    ctx->headers = curl_slist_append(ctx->headers, "Content-Type: application/json");
    gchar *auth = g_strdup_printf("X-Session-Auth: %s", ctx->sessionauth);
    ctx->headers = curl_slist_append(ctx->headers, auth);
    g_free(auth);

    // TODO: store ip somewhere

    // TODO: is this necessary?
    request->server->rawlog = rawlog_create();

    request->server->connect_tag = -1;
    server_connect_finished(SERVER(request->server));

    yajl_tree_free(root);
    return true;
}

static void retry_request(const char *target, gpointer userdata) {
    CURL *curl = userdata;
    struct t_robustirc_request *request = NULL;

    curl_easy_getinfo(curl, CURLINFO_PRIVATE, &request);

    printformat_module(MODULE_NAME, request->server, NULL,
                       MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_RETRY,
                       request->url_suffix, request->target, target);

    // Reset the HTTP body and parser state, if any.
    free(request->body->body);
    request->body->body = NULL;
    request->body->size = 0;
    if (request->type == RT_GETMESSAGES) {
        yajl_free(request->parser);
        yajl_handle hand = yajl_alloc(&gm_callbacks, NULL, request);
        yajl_config(hand, yajl_allow_multiple_values, 1);
        request->parser = hand;
    }

    g_free(request->target);
    request->target = g_strdup(target);

    gchar *url = NULL;
    if (request->type == RT_GETMESSAGES) {
        url = g_strdup_printf(
            "https://%s%s?lastseen=%s",
            request->target,
            request->url_suffix,
            request->ctx->lastseen);
        request->timeout_tag = g_timeout_add_seconds(
            60, get_messages_timeout, curl);
    } else {
        url = g_strdup_printf(
            "https://%s%s", request->target, request->url_suffix);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    g_free(url);
    curl_multi_add_handle(curl_handle, curl);
    request->ctx->curl_handles = g_list_append(request->ctx->curl_handles, curl);
    int running;
    curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0, &running);
}

// check_multi_info iterates through all curl handles, handling those that
// completed by either retrying the request (on temporary errors) or freeing
// the corresponding memory.
static void check_multi_info(void) {
    CURLMsg *message = NULL;
    struct t_robustirc_request *request = NULL;
    int pending;
    long http_code;

    while ((message = curl_multi_info_read(curl_handle, &pending))) {
        if (message->msg != CURLMSG_DONE)
            continue;

        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
        curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &http_code);
        const bool error = (message->data.result != CURLE_OK || http_code != 200);
        // Errors on the curl higher level (e.g. connection refused) are not
        // permanent, and neither are the 5xx HTTP error codes.
        const bool temporary_error = (message->data.result != CURLE_OK ||
                                      (http_code >= 500 && http_code < 600));

        // TODO: log a line for every terminated HTTP request into the rawlog (or is there a better one?)

        if (!request->server ||
            !request->server->connrec ||
            !request->server->connrec->address) {
            goto cleanup;
        }

        if (message->data.result != CURLE_OK) {
            printformat_module(MODULE_NAME, request->server, NULL,
                               MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                               request->curl_error_buf);
        }

        if (error) {
            robustsession_network_failed(
                request->server->connrec->address, request->target);
        } else {
            robustsession_network_succeeded(
                request->server->connrec->address, request->target);
        }

        if (error && temporary_error) {
            curl_multi_remove_handle(curl_handle, message->easy_handle);
            request->ctx->curl_handles = g_list_remove(request->ctx->curl_handles, message->easy_handle);
            if (request->type == RT_GETMESSAGES) {
                g_source_remove(request->timeout_tag);
            }

            robustsession_network_server(
                request->server->connrec->address,
                (request->type == RT_GETMESSAGES),
                retry_request,
                message->easy_handle);
            continue;
        }

        if (error && !temporary_error) {
            printtext(NULL, NULL, MSGLEVEL_CRAP, "TODO: handle permanent error, http code %d", http_code);
            // TODO: cause a server disconnect event and free memory! this error is unrecoverable.
            continue;
        }

        // TODO: check for Content-Location header and _network_prefer that
        // server. Requires setting CURLOPT_HEADERFUNCTION, see
        // http://curl.haxx.se/libcurl/c/CURLOPT_HEADERFUNCTION.html

        switch (request->type) {
            case RT_CREATESESSION:
                if (create_session_done(request, message->easy_handle)) {
                    robustsession_network_server(
                        request->server->connrec->address,
                        TRUE,
                        get_messages,
                        request->server);
                }
                break;
            case RT_POSTMESSAGE:
                break;
            case RT_GETMESSAGES:
                // Typically unreached: a GetMessages request will never
                // successfully return. Messages are extracted from the
                // never-ending JSON stream instead.
                g_source_remove(request->timeout_tag);
                break;
            default:
                assert(false);
        }

    cleanup:
        curl_multi_remove_handle(curl_handle, message->easy_handle);
        request->ctx->curl_handles = g_list_remove(request->ctx->curl_handles, message->easy_handle);
        curl_easy_cleanup(message->easy_handle);
        free(request->body->body);
        free(request->body);
        free(request);
    }
}

/* irssi callback which notifies libcurl about events on file descriptor |fd|. */
static void socket_recv_cb(void *data, GIOChannel *source, int condition) {
    int running;
    CURLMcode result = curl_multi_socket_action(
        curl_handle, g_io_channel_unix_get_fd(source), 0, &running);
    if (result != CURLM_OK) {
        printformat_module(MODULE_NAME, NULL, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                           curl_multi_strerror(result));
    }
    check_multi_info();
}

/* irssi callback which notifies libcurl about a timeout. */
static gboolean timeout_cb(gpointer user_data) {
    if (user_data) {
        g_free(user_data);
        curl_multi_setopt(curl_handle, CURLMOPT_TIMERDATA, NULL);
    }

    int running;
    CURLMcode result = curl_multi_socket_action(
        curl_handle, CURL_SOCKET_TIMEOUT, 0, &running);
    if (result != CURLM_OK) {
        printformat_module(MODULE_NAME, NULL, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                           curl_multi_strerror(result));
    }
    check_multi_info();
    return G_SOURCE_REMOVE;
}

/* libcurl callback which sets up a WeeChat hook to watch for events on socket |s|. */
static int socket_callback(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp) {
    if (what == CURL_POLL_NONE)
        return 0;

    guint *id = socketp;

    if (what == CURL_POLL_REMOVE) {
        if (id) {
            g_source_remove(*id);
            g_free(id);
            curl_multi_assign(curl_handle, s, NULL);
        }
        return 0;
    }

    if (!id)
        id = g_new(guint, 1);
    GIOChannel *handle = g_io_channel_new(s);
    int condition = 0;
    switch (what) {
        case CURL_POLL_IN:
            condition = G_INPUT_READ;
            break;
        case CURL_POLL_OUT:
            condition = G_INPUT_WRITE;
            break;
        case CURL_POLL_INOUT:
            condition = G_INPUT_READ | G_INPUT_WRITE;
            break;
    }
    *id = g_input_add(handle, condition, socket_recv_cb, NULL);
    g_io_channel_unref(handle);
    curl_multi_assign(curl_handle, s, id);
    return 0;
}

/* libcurl callback to adjust the timeout of our WeeChat timer. */
static int start_timeout(CURLM *multi, long timeout_ms, void *userp) {
    guint *id = userp;

    if (id)
        g_source_remove(*id);

    // -1 means we should just delete our timer.
    if (timeout_ms == -1) {
        g_free(id);
        id = NULL;
    } else {
        if (!id)
            id = g_new(guint, 1);
        *id = g_timeout_add(timeout_ms, timeout_cb, id);
    }
    curl_multi_setopt(multi, CURLMOPT_TIMERDATA, id);
    return 0;
}

static size_t write_func(void *contents, size_t size, size_t nmemb, void *userp) {
    // We can safely multiply size * nmemb without overflow checking because
    // curl_easy_setopt(3), section CURLOPT_WRITEFUNCTION specifies that (size
    // * nmemb) < CURL_MAX_WRITE_SIZE == 16 KiB.
    size_t realsize = size * nmemb;
    struct t_robustirc_request *request = userp;
    struct t_body_buffer *body_buffer = request->body;

    if ((SIZE_MAX - realsize - 1) < body_buffer->size) {
        return 0;
    }

    body_buffer->body = realloc(body_buffer->body, body_buffer->size + realsize + 1);
    if (body_buffer->body == NULL) {
        return 0;
    }

    memcpy(&(body_buffer->body[body_buffer->size]), contents, realsize);
    body_buffer->size += realsize;
    body_buffer->body[body_buffer->size] = 0;

    return realsize;
}

bool robustsession_init(void) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
        return false;

    if (!(curl_handle = curl_multi_init()))
        return false;

    curl_multi_setopt(curl_handle, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout);

    server_ctx = g_hash_table_new(NULL, NULL);

    return robustsession_network_init();
}

void robustsession_deinit(void) {
    curl_multi_cleanup(curl_handle);
    g_hash_table_unref(server_ctx);
}

static void curl_set_common_options(CURL *curl,
                                    struct t_robustsession_ctx *ctx,
                                    SERVER_REC *server,
                                    struct t_robustirc_request *request) {
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ROBUSTSESSION_USER_AGENT);
    if (ctx) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, ctx->headers);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_func);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, request);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, request);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, request->curl_error_buf);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,
                     (int)server->connrec->ssl_verify);

    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30);

    if (server->connrec->family) {
        long resolve = CURL_IPRESOLVE_V6;
        if (server->connrec->family == AF_INET) {
            resolve = CURL_IPRESOLVE_V4;
        }
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, resolve);
    }

    // TODO: set proxy options, see CURLOPT_PROXY and CURLOPT_PROXYUSERPWD in
    // libcurl, see server->connrec->proxy{,_password,_port} in irssi.
}

// Called once robustsession_network_server gave us an available server.
// Sends a CreateSession request.
static void robustsession_connect_target(const char *target,
                                         gpointer userdata) {
    IRC_SERVER_REC *server = userdata;
    CURL *curl = NULL;
    struct t_robustsession_ctx *ctx = NULL;

    if (!(ctx = g_hash_table_lookup(server_ctx, server))) {
        return;
    }

    if (!(curl = curl_easy_init())) {
        printformat_module(MODULE_NAME, server, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                           "curl_easy_init() failed. Out of memory?");
        return;
    }

    struct t_robustirc_request *request = g_new0(struct t_robustirc_request, 1);
    request->type = RT_CREATESESSION;
    request->body = g_new0(struct t_body_buffer, 1);
    request->server = SERVER(server);
    request->ctx = ctx;
    request->url_suffix = g_strdup("/robustirc/v1/session");
    request->target = g_strdup(target);
    gchar *url = g_strdup_printf(
        "https://%s%s",
        request->target,
        request->url_suffix);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    g_free(url);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_set_common_options(curl, ctx, SERVER(server), request);

    /* Make libcurl immediately start handling the request. */
    curl_multi_add_handle(curl_handle, curl);
    ctx->curl_handles = g_list_append(ctx->curl_handles, curl);
    int running;
    curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0, &running);
}

static void robustsession_connect_resolved(IRC_SERVER_REC *server) {
    robustsession_network_server(
        server->connrec->address,
        TRUE,
        robustsession_connect_target,
        server);
}

void robustsession_connect(IRC_SERVER_REC *server) {
    gchar *m = g_strdup_printf("server = %p, server->connrec = %p", server, server->connrec);
    printtext(NULL, NULL, MSGLEVEL_CRAP, "looking. server = %s", m);
    g_free(m);

    struct t_robustsession_ctx *ctx = g_new0(struct t_robustsession_ctx, 1);
    ctx->lastseen = g_strdup("0.0");
    g_hash_table_insert(server_ctx, server, ctx);

    robustsession_network_resolve(server, robustsession_connect_resolved);
    signal_emit("server looking", 1, server);
}

struct send_ctx {
    SERVER_REC *server;
    char *buffer;
};

static void robustsession_send_target(const char *target, gpointer callback) {
    struct send_ctx *send_ctx = callback;
    gchar *url = NULL;
    yajl_gen gen = NULL;
    CURL *curl = NULL;
    struct t_robustirc_request *request = NULL;
    struct t_robustsession_ctx *ctx = NULL;

    if (!(ctx = g_hash_table_lookup(server_ctx, send_ctx->server))) {
        goto error;
    }

    if (!(curl = curl_easy_init())) {
        printformat_module(MODULE_NAME, send_ctx->server, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                           "curl_easy_init() failed. Out of memory?");
        goto error;
    }

    if (!(gen = yajl_gen_alloc(NULL))) {
        printformat_module(MODULE_NAME, send_ctx->server, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                           "yajl_gen_alloc() failed. Out of memory?");
        goto error;
    }

    // TODO: yajl error handling
    yajl_gen_map_open(gen);
    yajl_gen_string(gen, (const unsigned char *)"Data", strlen("Data"));
    yajl_gen_string(gen, (const unsigned char *)send_ctx->buffer, strlen(send_ctx->buffer));
    yajl_gen_string(gen, (const unsigned char *)"ClientMessageId", strlen("ClientMessageId"));
    yajl_gen_integer(gen, g_str_hash(send_ctx->buffer) + rand());
    yajl_gen_map_close(gen);
    const unsigned char *body = NULL;
    size_t len = 0;
    yajl_gen_get_buf(gen, &body, &len);

    request = g_new0(struct t_robustirc_request, 1);
    request->type = RT_POSTMESSAGE;
    request->body = g_new0(struct t_body_buffer, 1);
    request->server = send_ctx->server;
    request->target = g_strdup(target);
    request->ctx = ctx;
    request->url_suffix = g_strdup_printf("/robustirc/v1/%s/message",
                                          ctx->sessionid);

    if (!(url = g_strdup_printf("https://%s%s", request->target, request->url_suffix))) {
        printformat_module(MODULE_NAME, send_ctx->server, NULL,
                           MSGLEVEL_CRAP, ROBUSTIRCTXT_ERROR_TEMPORARY,
                           "g_strdup_printf() failed. Out of memory?");
        goto error;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    g_free(url);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body);
    curl_set_common_options(curl, ctx, send_ctx->server, request);
    yajl_gen_free(gen);

    /* Make libcurl immediately start handling the request. */
    curl_multi_add_handle(curl_handle, curl);
    ctx->curl_handles = g_list_append(ctx->curl_handles, curl);
    int running;
    curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0, &running);

    free(send_ctx->buffer);
    free(send_ctx);
    return;

error:
    if (curl != NULL)
        curl_easy_cleanup(curl);
    if (gen != NULL)
        yajl_gen_free(gen);
    g_free(url);
    if (request != NULL) {
        free(request->body);
    }
    free(request);
    free(send_ctx->buffer);
    free(send_ctx);
}

void robustsession_send(SERVER_REC *server, const char *buffer, int size_buf) {
    struct send_ctx *ctx = g_new0(struct send_ctx, 1);
    ctx->server = server;
    ctx->buffer = g_strdup(buffer);
    robustsession_network_server(
        server->connrec->address,
        FALSE,
        robustsession_send_target,
        ctx);
}

void robustsession_destroy(SERVER_REC *server) {
    struct t_robustsession_ctx *ctx = NULL;

    if (!(ctx = g_hash_table_lookup(server_ctx, server))) {
        return;
    }

    // Abort all currently running requests. This prevents any callbacks from
    // triggering and trying to reference the server data which is about to be
    // freed.
    for (GList *h = ctx->curl_handles; h; h = h->next) {
        CURL *curl = h->data;
        // TODO: refactor cleanup into a separate function
        struct t_robustirc_request *request = NULL;
        curl_easy_getinfo(curl, CURLINFO_PRIVATE, &request);
        curl_multi_remove_handle(curl_handle, curl);
        curl_easy_cleanup(curl);

        if (request->type == RT_GETMESSAGES) {
            g_source_remove(request->timeout_tag);
        }

        free(request->body->body);
        free(request->body);
        free(request->target);
        free(request);
    }

    g_list_free(ctx->curl_handles);

    // TODO: send destroysession request, possibly only on a best-effort basis to avoid big refactoring.

    // TODO: free the _network entry if there are no other open connections to
    // that same network so that we’ll re-resolve the next time we connect.
    // maybe use refcounting for that? does that play nice with a hashtable?
}