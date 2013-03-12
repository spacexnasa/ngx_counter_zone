
/*
 * Copyright (C) Igor Sysoev
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    u_char              color;
    u_char              len;
    u_short             counter;
    u_char              data[1];
} ngx_http_counter_zone_node_t;


typedef struct {
    ngx_shm_zone_t     *shm_zone;
    ngx_rbtree_node_t  *node;
} ngx_http_counter_zone_cleanup_t;


typedef struct {
    ngx_http_complex_value_t cv;
    ngx_shm_zone_t     *shm_zone;
} ngx_http_counter_zone_var_t;


typedef struct {
    ngx_rbtree_t             *rbtree;
    ngx_http_complex_value_t *index;
} ngx_http_counter_zone_ctx_t;


typedef struct {
    ngx_uint_t          log_level;
    ngx_list_t         *shm_zone_list;
    ngx_list_t         *shm_zone_drop_list;
    ngx_http_complex_value_t *cv_arg;
    ngx_uint_t          cv_arg_val;
    ngx_http_complex_value_t *cv_drop;
} ngx_http_counter_zone_conf_t;



static void *ngx_http_counter_zone_create_conf(ngx_conf_t *cf);
static char *ngx_http_counter_zone_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_counter_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_counter(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_counter_get(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char * ngx_http_counter_drop(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_counter_zone_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_http_counter_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_http_counter_zone_commands[] = {

    { ngx_string("counter_zone"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE3,
      ngx_http_counter_zone,
      0,
      0,
      NULL },

    { ngx_string("counter"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_TAKE2,
      ngx_http_counter,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("counter_get"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE3,
      ngx_http_counter_get,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("counter_drop"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_http_counter_drop,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("counter_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_counter_zone_conf_t, log_level),
      &ngx_http_counter_log_levels },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_counter_zone_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_counter_zone_init,            /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_counter_zone_create_conf,       /* create location configration */
    ngx_http_counter_zone_merge_conf         /* merge location configration */
};


ngx_module_t  ngx_http_counter_zone_module = {
    NGX_MODULE_V1,
    &ngx_http_counter_zone_module_ctx,       /* module context */
    ngx_http_counter_zone_commands,          /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static uint32_t
ngx_get_hash(ngx_http_request_t *r, ngx_http_complex_value_t *cv, int unescape, uint32_t *hash, ngx_str_t *vv)
{
  if (ngx_http_complex_value(r, cv, vv) != NGX_OK)
    return NGX_DECLINED;

  if (vv->len == 0) {
    return NGX_DECLINED;
  }

  if (vv->len > 255) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
        "the value of the counter variable "
        "is more than 255 bytes: \"%V\"", vv);
    return NGX_DECLINED;
  }

  if (unescape) {
    u_char *dst, *src;
    dst=src=vv->data;
    ngx_unescape_uri(&dst, &src, vv->len, 0);
    vv->len = dst - vv->data;
  }

  *hash = ngx_crc32_short(vv->data, vv->len);

  return NGX_OK;
}

static ngx_int_t
ngx_http_counter_zone_handler(ngx_http_request_t *r)
{
  size_t                          n, cnt=0;
  uint32_t                        hash;
  ngx_int_t                       rc, val=0;
  ngx_slab_pool_t                *shpool;
  ngx_rbtree_node_t              *node, *sentinel;
  ngx_http_counter_zone_ctx_t      *ctx;
  ngx_http_counter_zone_node_t     *lz=NULL;
  ngx_http_counter_zone_conf_t     *lzcf;
  ngx_list_part_t     *part;
  ngx_shm_zone_t      *shm_zone;
  ngx_str_t vv;
  ngx_uint_t           i, drop;

  if (r->main->counter_zone_set) {
    return NGX_DECLINED;
  }

  lzcf = ngx_http_get_module_loc_conf(r, ngx_http_counter_zone_module);

  ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "http counter handler %p %p", lzcf->shm_zone_list, lzcf->shm_zone_drop_list);

  if (lzcf->shm_zone_list == NULL && lzcf->shm_zone_drop_list == NULL) {
    return NGX_DECLINED;
  }

  drop=(lzcf->shm_zone_drop_list!=NULL);

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "http counter drop %d", drop);

  r->main->counter_zone_set = 1;
  part = drop?&lzcf->shm_zone_drop_list->part:&lzcf->shm_zone_list->part;
  shm_zone = *(ngx_shm_zone_t**)part->elts;

  for (i = 0; /* void */ ; i++) {
    if (i >= part->nelts) {
      if (part->next == NULL) {
        break;
      }

      part = part->next;
      i = 0;
    }
    shm_zone = ((ngx_shm_zone_t**)part->elts)[i];
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "http counter for \"%V\" zone", &shm_zone->shm.name);

    ctx = shm_zone->data;

    if (drop) {
      ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
          "counter dropping");
      rc = ngx_get_hash(r, lzcf->cv_drop, 1, &hash, &vv);
      if (rc != NGX_OK)
        return rc;
    } else {
      ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
          "http counter processing");
      rc = ngx_get_hash(r, ctx->index, 0, &hash, &vv);
      if (rc != NGX_OK)
        return rc;
      ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
          "http counter hash \"%08XD\" %p", hash, lzcf->cv_arg);
      if (lzcf->cv_arg) {
        ngx_str_t vvv;
        if (ngx_http_complex_value(r, lzcf->cv_arg, &vvv) != NGX_OK)
          return NGX_DECLINED;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "http counter for \"%V\"", &vvv);
        val = ngx_atoi(vvv.data, vvv.len);
      } else {
        val=lzcf->cv_arg_val;
      }
      if (!val)
        return NGX_DECLINED;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

      ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
          "http counter shpool %p", shpool);
    ngx_shmtx_lock(&shpool->mutex);

    node = ctx->rbtree->root;
    sentinel = ctx->rbtree->sentinel;

    while (node != sentinel) {

      if (hash < node->key) {
        node = node->left;
        continue;
      }

      if (hash > node->key) {
        node = node->right;
        continue;
      }

      /* hash == node->key */

      do {
        lz = (ngx_http_counter_zone_node_t *) &node->color;

        rc = ngx_memn2cmp(vv.data, lz->data, vv.len, (size_t) lz->len);

        if (rc == 0) {
          if ( drop ) {
            cnt=lz->counter;
            ngx_rbtree_delete(ctx->rbtree, node);
            ngx_slab_free_locked(shpool, node);
          } else {
            lz->counter+=val;
            cnt=lz->counter;
          }
          goto done;
        }

        node = (rc < 0) ? node->left : node->right;

      } while (node != sentinel && hash == node->key);

      break;
    }

    if ( drop )
      goto done;
    n = offsetof(ngx_rbtree_node_t, color)
      + offsetof(ngx_http_counter_zone_node_t, data)
      + vv.len;

    node = ngx_slab_alloc_locked(shpool, n);
    if (node == NULL) {
      ngx_shmtx_unlock(&shpool->mutex);
      return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    lz = (ngx_http_counter_zone_node_t *) &node->color;

    node->key = hash;
    lz->len = (u_char) vv.len;
    lz->counter = val;
    cnt=lz->counter;
    ngx_memcpy(lz->data, vv.data, vv.len);

    ngx_rbtree_insert(ctx->rbtree, node);

done:

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "http counter zone: %s %08XD %d", drop?"dropped":"updated", hash, cnt);

#if 0
    if ( lzcf->drop )
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
          "http counter zone: %s %08XD %d", lzcf->drop?"dropped":"updated", hash, cnt);
#endif

    ngx_shmtx_unlock(&shpool->mutex);
  }

  return NGX_DECLINED;
}


static void
ngx_http_counter_zone_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t           **p;
    ngx_http_counter_zone_node_t   *lzn, *lznt;

    for ( ;; ) {

        if (node->key < temp->key) {

            p = &temp->left;

        } else if (node->key > temp->key) {

            p = &temp->right;

        } else { /* node->key == temp->key */

            lzn = (ngx_http_counter_zone_node_t *) &node->color;
            lznt = (ngx_http_counter_zone_node_t *) &temp->color;

            p = (ngx_memn2cmp(lzn->data, lznt->data, lzn->len, lznt->len) < 0)
                ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}


static ngx_int_t
ngx_http_counter_zone_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_counter_zone_ctx_t  *octx = data;

    size_t                      len;
    ngx_slab_pool_t            *shpool;
    ngx_rbtree_node_t          *sentinel;
    ngx_http_counter_zone_ctx_t  *ctx;

    ctx = shm_zone->data;

    if (octx) {
        ctx->rbtree = octx->rbtree;

        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->rbtree = shpool->data;

        return NGX_OK;
    }

    ctx->rbtree = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_t));
    if (ctx->rbtree == NULL) {
        return NGX_ERROR;
    }

    shpool->data = ctx->rbtree;

    sentinel = ngx_slab_alloc(shpool, sizeof(ngx_rbtree_node_t));
    if (sentinel == NULL) {
        return NGX_ERROR;
    }

    ngx_rbtree_init(ctx->rbtree, sentinel,
                    ngx_http_counter_zone_rbtree_insert_value);

    len = sizeof(" in counter_zone \"\"") + shm_zone->shm.name.len;

    shpool->log_ctx = ngx_slab_alloc(shpool, len);
    if (shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(shpool->log_ctx, " in counter_zone \"%V\"%Z",
                &shm_zone->shm.name);

    return NGX_OK;
}


static void *
ngx_http_counter_zone_create_conf(ngx_conf_t *cf)
{
    ngx_http_counter_zone_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_counter_zone_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    conf->log_level = NGX_CONF_UNSET_UINT;

    return conf;
}


    ngx_uint_t          log_level;
    ngx_list_t         *shm_zone_list;
    ngx_list_t         *shm_zone_drop_list;
    ngx_http_complex_value_t *cv_arg;
    ngx_uint_t          cv_arg_val;
    ngx_http_complex_value_t *cv_drop;
static char *
ngx_http_counter_zone_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_counter_zone_conf_t *prev = parent;
    ngx_http_counter_zone_conf_t *conf = child;

    ngx_conf_merge_uint_value(conf->log_level, prev->log_level, NGX_LOG_ERR);

    if (conf->shm_zone_list == NULL) {
        conf->shm_zone_list=prev->shm_zone_list;
    }

    if (conf->shm_zone_drop_list == NULL) {
        conf->shm_zone_drop_list = prev->shm_zone_drop_list;
    }

    if (conf->cv_arg == NULL) {
        conf->cv_arg = prev->cv_arg;
    }

    if (conf->cv_drop == NULL) {
        conf->cv_drop = prev->cv_drop;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_counter_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ssize_t                     n;
  ngx_str_t                  *value;
  ngx_shm_zone_t             *shm_zone;
  ngx_http_complex_value_t cv;
  ngx_http_compile_complex_value_t ccv;
  ngx_http_counter_zone_ctx_t  *ctx=NULL;

  value = cf->args->elts;

  n = ngx_parse_size(&value[3]);

  if (n == NGX_ERROR) {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
        "invalid size of counter_zone \"%V\"", &value[3]);
    return NGX_CONF_ERROR;
  }

  if (n < (ngx_int_t) (8 * ngx_pagesize)) {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
        "counter_zone \"%V\" is too small", &value[1]);
    return NGX_CONF_ERROR;
  }


  shm_zone = ngx_shared_memory_add(cf, &value[1], n,
      &ngx_http_counter_zone_module);
  if (shm_zone == NULL) {
    return NGX_CONF_ERROR;
  }

  ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_counter_zone_ctx_t));
  if (ctx == NULL) {
    return NGX_CONF_ERROR;
  }

  if (shm_zone->data) {
    ctx = shm_zone->data;

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
        "counter_zone \"%V\" is already bound to variable",
        &value[1]);
    return NGX_CONF_ERROR;
  }

  ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

  ccv.cf = cf;
  ccv.value = &value[2];
  ccv.complex_value = &cv;

  if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
    return NGX_CONF_ERROR;
  }

  if (ctx && cv.lengths != NULL) {
    ctx->index=ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
    if (ctx->index == NULL) {
      return NGX_CONF_ERROR;
    }

    *ctx->index = cv;
  }

  shm_zone->init = ngx_http_counter_zone_init_zone;
  shm_zone->data = ctx;

  return NGX_CONF_OK;
}


static char *
ngx_http_counter(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_counter_zone_conf_t  *lzcf = conf;
  ngx_http_complex_value_t cv;
  ngx_http_compile_complex_value_t ccv;
  ngx_http_counter_zone_ctx_t      *ctx;
  ngx_shm_zone_t             *shm_zone, **tzone;

  ngx_str_t  *value;

  value = cf->args->elts;

  shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
      &ngx_http_counter_zone_module);

  if (shm_zone == NULL) {
    return NGX_CONF_ERROR;
  }

  if (!lzcf->shm_zone_list) {
    lzcf->shm_zone_list=ngx_list_create(cf->pool, 4,
        sizeof(ngx_shm_zone_t*));
    if (!lzcf->shm_zone_list) {
      return NGX_CONF_ERROR;
    }
  }
  tzone=ngx_list_push(lzcf->shm_zone_list);
  if (!tzone)
    return NGX_CONF_ERROR;
  *tzone=shm_zone;

  ctx = shm_zone->data;

  ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

  ccv.cf = cf;
  ccv.value = &value[2];
  ccv.complex_value = &cv;

  if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
    return NGX_CONF_ERROR;
  }

  if (cv.lengths != NULL) {
    lzcf->cv_arg=ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
    if (lzcf->cv_arg == NULL) {
      return NGX_CONF_ERROR;
    }

    *lzcf->cv_arg = cv;
  } else {
    lzcf->cv_arg_val=ngx_atoi(value[2].data, value[2].len);
  }

  return NGX_CONF_OK;
}


static int
ngx_http_get_id(ngx_http_request_t *r,
    ngx_http_counter_zone_var_t *var_ctx, ngx_str_t *value)
{
  ngx_str_t      vv;
  size_t                          ret=0;
  uint32_t                        hash;
  ngx_int_t                       rc;
  ngx_slab_pool_t                *shpool;
  ngx_rbtree_node_t              *node, *sentinel;
  ngx_http_counter_zone_ctx_t      *ctx;
  ngx_http_counter_zone_node_t     *lz;
  ngx_http_counter_zone_conf_t     *lzcf;

  lzcf = ngx_http_get_module_loc_conf(r, ngx_http_counter_zone_module);

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "http counter var %p", var_ctx->shm_zone);

  if (!value)
    return NGX_DECLINED;

  if (var_ctx->shm_zone == NULL) {
    return NGX_DECLINED;
  }

  ctx = var_ctx->shm_zone->data;

  rc = ngx_get_hash(r, &var_ctx->cv, 1, &hash, &vv);
  if (rc != NGX_OK)
    return rc;

  shpool = (ngx_slab_pool_t *) var_ctx->shm_zone->shm.addr;

  ngx_shmtx_lock(&shpool->mutex);

  node = ctx->rbtree->root;
  sentinel = ctx->rbtree->sentinel;

  while (node != sentinel) {

  ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "http counter HASH %08XD %08XD", node->key, hash);

    if (hash < node->key) {
      node = node->left;
      continue;
    }

    if (hash > node->key) {
      node = node->right;
      continue;
    }

    /* hash == node->key */

    do {
      lz = (ngx_http_counter_zone_node_t *) &node->color;

      rc = ngx_memn2cmp(vv.data, lz->data, vv.len, (size_t) lz->len);

      if (rc == 0) {
        ret=lz->counter;
        goto done;
      }

      node = (rc < 0) ? node->left : node->right;

    } while (node != sentinel && hash == node->key);

    break;
  }

  ngx_shmtx_unlock(&shpool->mutex);
  return NGX_DECLINED;
done:

  ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
      "http counter get zone: %08XD %d", node->key, lz->counter);

  ngx_shmtx_unlock(&shpool->mutex);

  value->data=ngx_palloc(r->pool, 100);
  ngx_sprintf(value->data, "%d%Z", ret);
  value->len=ngx_strlen(value->data);

  return NGX_OK;
}

static ngx_int_t
ngx_http_counter_get_handler(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
  ngx_http_counter_zone_var_t *cv = (ngx_http_counter_zone_var_t *) data;
  ngx_str_t   value=ngx_string("");

  if (ngx_http_get_id(r, cv, &value) != NGX_OK) {
    v->not_found = 1;
    return NGX_OK;
  }

  v->data = value.data;
  v->len = value.len;
  v->valid = 1;
  v->no_cacheable = 0;
  v->not_found = 0;

  return NGX_OK;
}

static char *
ngx_http_counter_get(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_complex_value_t cv;
  ngx_http_compile_complex_value_t ccv;
  ngx_shm_zone_t     *shm_zone;

  ngx_str_t  *value;

  value = cf->args->elts;

  if (value[2].data[0] != '$') {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
        "invalid variable name \"%V\"", &value[2]);
    return NGX_CONF_ERROR;
  }

  value[2].len--;
  value[2].data++;

  ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

  ccv.cf = cf;
  ccv.value = &value[3];
  ccv.complex_value = &cv;

  if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
    return NGX_CONF_ERROR;
  }


  if (cv.lengths == NULL) {
    return NGX_CONF_ERROR;
  }

  ngx_http_variable_t        *v;
  ngx_http_counter_zone_var_t *var_ctx=ngx_palloc(cf->pool, sizeof(ngx_http_counter_zone_var_t));
  if (var_ctx == NULL) {
    return NGX_CONF_ERROR;
  }

  shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
      &ngx_http_counter_zone_module);
  if (shm_zone == NULL) {
    return NGX_CONF_ERROR;
  }

  var_ctx->cv = cv;
  var_ctx->shm_zone=shm_zone;
  v=ngx_http_add_variable(cf, &value[2], NGX_HTTP_VAR_NOCACHEABLE);
  v->get_handler = ngx_http_counter_get_handler;
  v->data=(uintptr_t)var_ctx;

  return NGX_CONF_OK;
}


static char *
ngx_http_counter_drop(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_counter_zone_conf_t  *lzcf = conf;
  ngx_http_complex_value_t cv;
  ngx_http_compile_complex_value_t ccv;
  ngx_str_t  *value;
  ngx_shm_zone_t     *shm_zone, **tzone;

  value = cf->args->elts;

  shm_zone = ngx_shared_memory_add(cf, &value[1], 0,
      &ngx_http_counter_zone_module);
  if (shm_zone == NULL) {
    return NGX_CONF_ERROR;
  }

  if (!lzcf->shm_zone_drop_list) {
    lzcf->shm_zone_drop_list=ngx_list_create(cf->pool, 4,
        sizeof(ngx_shm_zone_t*));
    if (!lzcf->shm_zone_drop_list) {
      return NGX_CONF_ERROR;
    }
  }
  tzone=ngx_list_push(lzcf->shm_zone_drop_list);
  if (!tzone)
    return NGX_CONF_ERROR;
  *tzone=shm_zone;

  ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));

  ccv.cf = cf;
  ccv.value = &value[2];
  ccv.complex_value = &cv;

  if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
    return NGX_CONF_ERROR;
  }

  if (cv.lengths != NULL) {
   lzcf->cv_drop=ngx_palloc(cf->pool, sizeof(ngx_http_complex_value_t));
    if (lzcf->cv_drop == NULL) {
      return NGX_CONF_ERROR;
    }

    *lzcf->cv_drop = cv;
  }

  return NGX_CONF_OK;
}


static void *
ngx_array_ins(ngx_array_t *a)
{
    void        *elt, *new;
    size_t       size;
    ngx_pool_t  *p;

#if 0
    if (a->nelts == a->nalloc) {

        /* the array is full */

#endif
        size = a->size * a->nalloc;

        p = a->pool;

#if 0
        if ((u_char *) a->elts + size == p->d.last
            && p->d.last + a->size <= p->d.end)
        {
            /*
             * the array allocation is the last in the pool
             * and there is space for new allocation
             */

            p->d.last += a->size;
            a->nalloc++;

        } else {
#endif
            /* allocate a new array */

            new = ngx_palloc(p, 2 * size);
            if (new == NULL) {
                return NULL;
            }

            ngx_memcpy((u_char *)new+a->size, a->elts, size);
            a->elts = new;
            a->nalloc *= 2;
#if 0
        }
    }

    elt = (u_char *) a->elts + a->size * a->nelts;
#endif
    elt = a->elts;
    a->nelts++;

    return elt;
}

static ngx_int_t
ngx_http_counter_zone_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

//    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    h = ngx_array_ins(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_counter_zone_handler;
    return NGX_OK;
}
