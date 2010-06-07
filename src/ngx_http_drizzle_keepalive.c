/* Copyright (C) Maxim Dounin */
/* Copyright (C) agentzh */

#define DDEBUG 0
#include "ddebug.h"

#include "ngx_http_drizzle_keepalive.h"
#include "ngx_http_drizzle_util.h"

#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>

static void ngx_http_drizzle_keepalive_dummy_handler(ngx_event_t *ev);

static void ngx_http_drizzle_keepalive_close_handler(ngx_event_t *ev);


char *
ngx_http_upstream_drizzle_keepalive(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf)
{
    ngx_http_upstream_drizzle_srv_conf_t        *dscf = conf;
    ngx_str_t                                   *value;
    ngx_uint_t                                   i;
    ngx_int_t                                    n;
    u_char                                      *data;
    ngx_uint_t                                   len;

    if (dscf->max_cached) {
        return "is duplicate";
    }

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "max=", sizeof("max=") - 1)
                == 0)
        {
            len = value[i].len - (sizeof("max=") - 1);
            data = &value[i].data[sizeof("max=") - 1];

            n = ngx_atoi(data, len);

            if (n == NGX_ERROR || n < 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid \"max\" value \"%V\" "
                                   "in \"%V\" directive",
                                   &value[i], &cmd->name);

                return NGX_CONF_ERROR;
            }

            dscf->max_cached = n;

            continue;
        }

        if (ngx_strncmp(value[i].data, "mode=", sizeof("mode=") - 1)
                == 0)
        {
            len = value[i].len - (sizeof("mode=") - 1);
            data = &value[i].data[sizeof("mode=") - 1];

            switch (len) {
            case 6:
                if (ngx_str6cmp(data, 's', 'i', 'n', 'g', 'l', 'e')) {
                    dscf->single = 1;
                }
                break;

            case 5:
                if (ngx_str5cmp(data, 'm', 'u', 'l', 't', 'i')) {
                    dscf->single = 0;
                }
                break;

            default:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "drizzle: invalid \"mode\" value \"%V\" "
                                   "in \"%V\" directive",
                                   &value[i], &cmd->name);

                return NGX_CONF_ERROR;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "overflow=", sizeof("overflow=") - 1)
                == 0)
        {
            len = value[i].len - (sizeof("overflow=") - 1);
            data = &value[i].data[sizeof("overflow=") - 1];

            switch (len) {
            case 6:
                if (ngx_str6cmp(data, 'r', 'e', 'j', 'e', 'c', 't')) {
                    dscf->overflow = drizzle_keepalive_overflow_reject;
                } else if (ngx_str6cmp(data, 'i', 'g', 'n', 'o', 'r', 'e')) {
                    dscf->overflow = drizzle_keepalive_overflow_ignore;
                }
                break;

            default:
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "drizzle: invalid \"overflow\" value \"%V\" "
                                   "in \"%V\" directive",
                                   &value[i], &cmd->name);

                return NGX_CONF_ERROR;
            }

            continue;
        }

        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "drizzle: invalid parameter \"%V\" in"
                           " \"%V\" directive",
                           &value[i], &cmd->name);

        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


ngx_int_t
ngx_http_drizzle_keepalive_init(ngx_pool_t *pool,
        ngx_http_upstream_drizzle_srv_conf_t *dscf)
{
    ngx_uint_t                              i;
    ngx_http_drizzle_keepalive_cache_t     *cached;

    /* allocate cache items and add to free queue */

    cached = ngx_pcalloc(pool,
                sizeof(ngx_http_drizzle_keepalive_cache_t) * dscf->max_cached);
    if (cached == NULL) {
        return NGX_ERROR;
    }

    ngx_queue_init(&dscf->cache);
    ngx_queue_init(&dscf->free);

    for (i = 0; i < dscf->max_cached; i++) {
        ngx_queue_insert_head(&dscf->free, &cached[i].queue);
        cached[i].srv_conf = dscf;
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_drizzle_keepalive_get_peer_single(ngx_peer_connection_t *pc,
        ngx_http_upstream_drizzle_peer_data_t *dp,
        ngx_http_upstream_drizzle_srv_conf_t *dscf)
{
    ngx_http_drizzle_keepalive_cache_t      *item;
    ngx_queue_t                             *q;
    ngx_connection_t                        *c;

    if (! ngx_queue_empty(&dscf->cache)) {

        q = ngx_queue_head(&dscf->cache);
        ngx_queue_remove(q);

        item = ngx_queue_data(q, ngx_http_drizzle_keepalive_cache_t, queue);
        c = item->connection;

        ngx_queue_insert_head(&dscf->free, q);

        c->idle = 0;
        c->log = pc->log;
        c->read->log = pc->log;
        c->write->log = pc->log;

        pc->connection = c;
        pc->cached = 1;

        dp->name = &item->name;
        dp->drizzle_con = item->drizzle_con;

        return NGX_DONE;
    }

    return NGX_DECLINED;
}


ngx_int_t
ngx_http_drizzle_keepalive_get_peer_multi(ngx_peer_connection_t *pc,
        ngx_http_upstream_drizzle_peer_data_t *dp,
        ngx_http_upstream_drizzle_srv_conf_t *dscf)
{
    ngx_queue_t                             *q, *cache;
    ngx_http_drizzle_keepalive_cache_t      *item;
    ngx_connection_t                        *c;

    /* search cache for suitable connection */

    cache = &dscf->cache;

    for (q = ngx_queue_head(cache);
         q != ngx_queue_sentinel(cache);
         q = ngx_queue_next(q))
    {
        item = ngx_queue_data(q, ngx_http_drizzle_keepalive_cache_t, queue);
        c = item->connection;

        /* XXX maybe we should take dbname and user into account
         * as well? */
        if (ngx_memn2cmp((u_char *) &item->sockaddr, (u_char *) pc->sockaddr,
                         item->socklen, pc->socklen)
                == 0)
        {
            ngx_queue_remove(q);
            ngx_queue_insert_head(&dscf->free, q);

            c->idle = 0;
            c->log = pc->log;
            c->read->log = pc->log;
            c->write->log = pc->log;

            pc->connection = c;
            pc->cached = 1;

            dp->name = &item->name;
            dp->drizzle_con = item->drizzle_con;

            return NGX_DONE;
        }
    }

    return NGX_DECLINED;
}


void
ngx_http_drizzle_keepalive_free_peer(ngx_peer_connection_t *pc,
        ngx_http_upstream_drizzle_peer_data_t *dp,
        ngx_http_upstream_drizzle_srv_conf_t *dscf,
        ngx_uint_t  state)
{
    ngx_uint_t                               status;
    ngx_http_drizzle_keepalive_cache_t      *item;
    ngx_queue_t                             *q;
    ngx_connection_t                        *c;
    ngx_http_upstream_t                     *u;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                   "drizzle: free keepalive peer");

    if (state & NGX_PEER_FAILED) {
        dp->failed = 1;
    }

    u = dp->upstream;
    status = u->headers_in.status_n;

    if (!dp->failed
        && pc->connection != NULL
        && (status == NGX_HTTP_NOT_FOUND
            || (status == NGX_HTTP_OK && u->header_sent && u->length == 0)))
    {
        c = pc->connection;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, pc->log, 0,
                       "drizzle: free keepalive peer: saving connection %p",
                       c);

        if (ngx_queue_empty(&dscf->free)) {
            /* connection pool is already full */

            q = ngx_queue_last(&dscf->cache);
            ngx_queue_remove(q);

            item = ngx_queue_data(q, ngx_http_drizzle_keepalive_cache_t,
                                  queue);

            ngx_http_upstream_drizzle_free_connection(pc->log, item->connection,
                    item->drizzle_con, dscf);

        } else {
            q = ngx_queue_head(&dscf->free);
            ngx_queue_remove(q);

            item = ngx_queue_data(q, ngx_http_drizzle_keepalive_cache_t,
                                  queue);
        }

        item->connection = c;
        ngx_queue_insert_head(&dscf->cache, q);

        pc->connection = NULL;

        if (c->read->timer_set) {
            ngx_del_timer(c->read);
        }
        if (c->write->timer_set) {
            ngx_del_timer(c->write);
        }

        c->write->handler = ngx_http_drizzle_keepalive_dummy_handler;
        c->read->handler = ngx_http_drizzle_keepalive_close_handler;

        c->data = item;
        c->idle = 1;
        c->log = ngx_cycle->log;
        c->read->log = ngx_cycle->log;
        c->write->log = ngx_cycle->log;

        item->socklen = pc->socklen;
        ngx_memcpy(&item->sockaddr, pc->sockaddr, pc->socklen);

        item->drizzle_con = dp->drizzle_con;
    }
}


static void
ngx_http_drizzle_keepalive_dummy_handler(ngx_event_t *ev)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0,
                   "drizzle: keepalive dummy handler");
}


static void
ngx_http_drizzle_keepalive_close_handler(ngx_event_t *ev)
{
    ngx_http_upstream_drizzle_srv_conf_t    *dscf;
    ngx_http_drizzle_keepalive_cache_t      *item;
    ngx_connection_t                        *c;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ev->log, 0,
                   "drizzle: keepalive close handler");

    c = ev->data;
    item = c->data;
    dscf = item->srv_conf;

    ngx_queue_remove(&item->queue);

    ngx_http_upstream_drizzle_free_connection(ev->log, item->connection,
            item->drizzle_con, dscf);

    ngx_queue_insert_head(&dscf->free, &item->queue);
}

