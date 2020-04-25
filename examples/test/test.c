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
#include <lib-common/datetime.h>


static struct {
    bool is_closing;

    httpc_cfg_t cfg;
    httpc_t *client;
    lstr_t host;
    lstr_t uri;

    bool opt_help;
    bool opt_version;
    int  fire;
    int  ok;
    int  error;
    uint64_t min;
    uint64_t max;
    uint64_t sum;
    uint64_t count;

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
    e_info("Client created");
    httpc_cfg_init(&_G.cfg);
    _G.cfg.refcnt++;
    _G.cfg.max_queries = 100;
    my_addr_resolve(s, &su);
    _G.client = httpc_connect(&su, &_G.cfg, NULL);
}

static int query_on_hdrs(httpc_query_t *q) {
    return 0;
}
static int query_on_data(httpc_query_t *q, pstream_t ps)
{
    uint64_t response_time;
    if (q->qinfo) {
        if (q->qinfo->code >= 200 && q->qinfo->code < 400) {
            _G.ok++;
        } else {
            _G.error++;
        }
    }
    response_time = lp_getmsec() - (uint64_t)q->pdata;
    _G.min = MIN(_G.min, response_time);
    _G.max = MAX(_G.max, response_time);
    _G.sum += response_time;
    _G.count++;
    return 0;
}
static void query_on_done(httpc_query_t *q, httpc_status_t status)
{
}

static int fire(void)
{
    httpc_query_t *query = malloc(sizeof(httpc_query_t));
    _G.fire++;
    if (!_G.client || !_G.client->ev) {
        e_error("No client");
        client_create(_G.uri.s);
        return -1;
    }
    if (_G.client->max_queries <= 0) {
        e_error("%d", _G.client->max_queries);
        client_create(_G.uri.s);
        return -1;
    }
    httpc_query_init(query);
    httpc_bufferize(query, 40 << 20);
    query->on_done = &query_on_done;
    query->on_data = &query_on_data;
    query->on_hdrs = &query_on_hdrs;

    httpc_query_attach(query, _G.client);
    httpc_query_start(query, HTTP_METHOD_GET, _G.host, LSTR_IMMED_V("/"));
    httpc_query_hdrs_done(query, 0, false);
    httpc_query_done(query);
    query->pdata = (void *)lp_getmsec();
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
    exit(0);
}

static void my_kill(el_t elh, data_t priv)
{
    exit(0);
}
static void stats(el_t elh, data_t priv)
{
    e_info("%03d %03d %03d %03ju %03ju %03ju", _G.fire, _G.ok, _G.error,
           _G.min, _G.count ? _G.sum/_G.count:0, _G.max);
    _G.fire = _G.ok = _G.error = 0;
}
static void slice_hook(el_t elh, data_t priv)
{
    fire();
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
    _G.min = UINT64_MAX;
    client_create(argv[0]);
    fire();
    el_signal_register(SIGTERM, &on_term, NULL);
    el_signal_register(SIGINT,  &on_term, NULL);
    el_signal_register(SIGQUIT, &on_term, NULL);
    el_timer_register(2000, 10, EL_TIMER_LOWRES, &slice_hook, NULL);
    el_timer_register(0, 1000, EL_TIMER_LOWRES, &stats, NULL);
    el_timer_register(30000, 0, EL_TIMER_LOWRES, &my_kill, NULL);


    /* got into event loop */
    el_loop();

    return 0;
}

/* }}} */
