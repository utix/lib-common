/***************************************************************************/
/*                                                                         */
/* Copyright 2020 François Lodier & Aurélien Lajoie                        */
/*                                                                         */
/* Licensed under the Apache License, Version 2.0 (the "License");         */
/* you may not use this file except in compliance with the License.        */
/* You may obtain a copy of the License at                                 */
/*                                                                         */
/*     http://www.apache.org/licenses/LICENSE-2.0                          */
/*                                                                         */
/* Unless required by applicable law or agreed to in writing, software     */
/* distributed under the License is distributed on an "AS IS" BASIS,       */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*/
/* See the License for the specific language governing permissions and     */
/* limitations under the License.                                          */
/*                                                                         */
/***************************************************************************/

#include <lib-common/parseopt.h>
#include <lib-common/el.h>
#include <lib-common/http.h>


static struct {
    bool is_closing;
    el_t blocker;

    httpc_cfg_t cfg;
    httpc_t *client;
    lstr_t host;
    lstr_t uri;

    bool opt_help;
    bool opt_version;
    int  fire;
    int  ok;
    int  error;

} _G;

/* {{{ utils */

static void my_addr_resolve(const char *s, sockunion_t *out)
{
    pstream_t host;
    in_port_t port;

    if (addr_parse(ps_initstr(s), &host, &port, -1))
        e_fatal("unable to parse address: %s", s);
    if (addr_info(out, AF_UNSPEC, host, port))
        e_fatal("unable to resolve address: %s", s);
    _G.host = LSTR_PS_V(&host);
    _G.uri = LSTR(s);
}

/* }}} */
/*{{{ client */
static void client_create(const char *s)
{
    sockunion_t su;
    httpc_cfg_init(&_G.cfg);
    _G.cfg.refcnt++;
    my_addr_resolve(s, &su);
    _G.client = httpc_connect(&su, &_G.cfg, NULL);
}

static void query_on_done(httpc_query_t *q, httpc_status_t status)
{
    if (status == HTTPC_STATUS_OK) {
        _G.ok++;
    } else {
        _G.error++;
    }
}

static int fire(void)
{
    httpc_query_t query;
    httpc_query_init(&query);
    httpc_bufferize(&query, 40 << 20);
    query.on_done = &query_on_done;

    httpc_query_attach(&query, _G.client);
    httpc_query_start(&query, HTTP_METHOD_GET, _G.host, _G.uri);
    httpc_query_hdrs_done(&query, 0, false);
    httpc_query_done(&query);
    return 0;
}
/*}}}*/
/* {{{ initialize & shutdown */

static popt_t popts[] = {
    OPT_GROUP("Options:"),
    OPT_FLAG('h', "help",    &_G.opt_help,     "show this help"),
    OPT_FLAG('v', "version", &_G.opt_version,  "show version"),
    OPT_END(),
};

static void on_term(el_t idx, int signum, data_t priv)
{
    if (_G.is_closing)
        return;

    /* Make event loop to stop */
    el_unregister(&_G.blocker);

    _G.is_closing = true;
}

int main(int argc, char **argv)
{
    const char *arg0 = NEXTARG(argc, argv);

    /* Read command line */
    argc = parseopt(argc, argv, popts, 0);
    if (_G.opt_help || argc != 1) {
        makeusage(_G.opt_help ? EXIT_SUCCESS : EXIT_FAILURE,
                  arg0, "<address>", NULL, popts);
    }

    if (_G.opt_version) {
        e_notice("HELLO - Version 1.0");
        return EXIT_SUCCESS;
    }
    /* Register signals & blocker */
    _G.blocker = el_blocker_register();
    el_signal_register(SIGTERM, &on_term, NULL);
    el_signal_register(SIGINT,  &on_term, NULL);
    el_signal_register(SIGQUIT, &on_term, NULL);

    client_create(argv[0]);
    fire();

    /* got into event loop */
    el_loop();

    return 0;
}

/* }}} */
