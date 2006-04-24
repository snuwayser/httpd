/* Copyright 1999-2004 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * mod_dir.c: handle default index files, and trailing-/ redirects
 */

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_request.h"
#include "http_protocol.h"
#include "http_log.h"
#include "http_main.h"
#include "util_script.h"

module MODULE_VAR_EXPORT dir_module;

typedef struct dir_config_struct {
    array_header *index_names;
} dir_config_rec;

#define DIR_CMD_PERMS OR_INDEXES

static const char *add_index(cmd_parms *cmd, void *dummy, char *arg)
{
    dir_config_rec *d = dummy;

    if (!d->index_names) {
	d->index_names = ap_make_array(cmd->pool, 2, sizeof(char *));
    }
    *(char **)ap_push_array(d->index_names) = arg;
    return NULL;
}

static const command_rec dir_cmds[] =
{
    {"DirectoryIndex", add_index, NULL,
     DIR_CMD_PERMS, ITERATE,
     "a list of file names"},
    {NULL}
};

static void *create_dir_config(pool *p, char *dummy)
{
    dir_config_rec *new =
    (dir_config_rec *) ap_pcalloc(p, sizeof(dir_config_rec));

    new->index_names = NULL;
    return (void *) new;
}

static void *merge_dir_configs(pool *p, void *basev, void *addv)
{
    dir_config_rec *new = (dir_config_rec *) ap_pcalloc(p, sizeof(dir_config_rec));
    dir_config_rec *base = (dir_config_rec *) basev;
    dir_config_rec *add = (dir_config_rec *) addv;

    new->index_names = add->index_names ? add->index_names : base->index_names;
    return new;
}

static int handle_dir(request_rec *r)
{
    dir_config_rec *d =
    (dir_config_rec *) ap_get_module_config(r->per_dir_config,
                                         &dir_module);
    char *dummy_ptr[1];
    char **names_ptr;
    int num_names;
    int error_notfound = 0;

    if (r->uri[0] == '\0' || r->uri[strlen(r->uri) - 1] != '/') {
        char *ifile;
        if (r->args != NULL)
            ifile = ap_pstrcat(r->pool, ap_escape_uri(r->pool, r->uri),
                            "/", "?", r->args, NULL);
        else
            ifile = ap_pstrcat(r->pool, ap_escape_uri(r->pool, r->uri),
                            "/", NULL);

        ap_table_setn(r->headers_out, "Location",
                  ap_construct_url(r->pool, ifile, r));
        return HTTP_MOVED_PERMANENTLY;
    }

    /* KLUDGE --- make the sub_req lookups happen in the right directory.
     * Fixing this in the sub_req_lookup functions themselves is difficult,
     * and would probably break virtual includes...
     */

    if (r->filename[strlen(r->filename) - 1] != '/') {
        r->filename = ap_pstrcat(r->pool, r->filename, "/", NULL);
    }

    if (d->index_names) {
	names_ptr = (char **)d->index_names->elts;
	num_names = d->index_names->nelts;
    }
    else {
	dummy_ptr[0] = DEFAULT_INDEX;
	names_ptr = dummy_ptr;
	num_names = 1;
    }

    for (; num_names; ++names_ptr, --num_names) {
        char *name_ptr = *names_ptr;
        request_rec *rr = ap_sub_req_lookup_uri(name_ptr, r);

        if (rr->status == HTTP_OK && S_ISREG(rr->finfo.st_mode)) {
            char *new_uri = ap_escape_uri(r->pool, rr->uri);

            if (rr->args != NULL)
                new_uri = ap_pstrcat(r->pool, new_uri, "?", rr->args, NULL);
            else if (r->args != NULL)
                new_uri = ap_pstrcat(r->pool, new_uri, "?", r->args, NULL);

            ap_destroy_sub_req(rr);
            ap_internal_redirect(new_uri, r);
            return OK;
        }

        /* If the request returned a redirect, propagate it to the client */

        if (ap_is_HTTP_REDIRECT(rr->status) ||
            (rr->status == HTTP_NOT_ACCEPTABLE && num_names == 1) ||
            (rr->status == HTTP_UNAUTHORIZED && num_names == 1)) {

            ap_pool_join(r->pool, rr->pool);
            error_notfound = rr->status;
            r->notes = ap_overlay_tables(r->pool, r->notes, rr->notes);
            r->headers_out = ap_overlay_tables(r->pool, r->headers_out,
                                            rr->headers_out);
            r->err_headers_out = ap_overlay_tables(r->pool, r->err_headers_out,
                                                rr->err_headers_out);
            return error_notfound;
        }

        /* If the request returned something other than 404 (or 200),
         * it means the module encountered some sort of problem. To be
         * secure, we should return the error, rather than create
         * along a (possibly unsafe) directory index.
         *
         * So we store the error, and if none of the listed files
         * exist, we return the last error response we got, instead
         * of a directory listing.
         */
        if (rr->status && rr->status != HTTP_NOT_FOUND && rr->status != HTTP_OK)
            error_notfound = rr->status;

        ap_destroy_sub_req(rr);
    }

    if (error_notfound)
        return error_notfound;

    if (r->method_number != M_GET)
        return DECLINED;

    /* nothing for us to do, pass on through */

    return DECLINED;
}


static const handler_rec dir_handlers[] =
{
    {DIR_MAGIC_TYPE, handle_dir},
    {NULL}
};

module MODULE_VAR_EXPORT dir_module =
{
    STANDARD_MODULE_STUFF,
    NULL,                       /* initializer */
    create_dir_config,          /* dir config creater */
    merge_dir_configs,          /* dir merger --- default is to override */
    NULL,                       /* server config */
    NULL,                       /* merge server config */
    dir_cmds,                   /* command table */
    dir_handlers,               /* handlers */
    NULL,                       /* filename translation */
    NULL,                       /* check_user_id */
    NULL,                       /* check auth */
    NULL,                       /* check access */
    NULL,                       /* type_checker */
    NULL,                       /* fixups */
    NULL,                       /* logger */
    NULL,                       /* header parser */
    NULL,                       /* child_init */
    NULL,                       /* child_exit */
    NULL                        /* post read-request */
};
