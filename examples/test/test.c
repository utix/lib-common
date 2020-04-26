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


typedef struct stats_t {
    uint64_t min;
    uint64_t max;
    uint64_t sum;
    uint64_t count;
} stats_t;
GENERIC_INIT(stats_t, stats);

static struct {
    bool is_closing;

    httpc_t *client;
    lstr_t   host;
    lstr_t   uri;

    bool     opt_help;
    bool     opt_version;
    int      fire;
    int      ok;
    int      error;


    stats_t  stat;
    stats_t  stat_global;
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

static void compute_stats(stats_t *s, uint64_t response_time)
{
    if (!s->count) {
        s->min = response_time;
    } else {
        s->min = MIN(s->min, response_time);
    }
    s->max = MAX(s->max, response_time);
    s->sum += response_time;
    s->count++;
}
/* }}} */
/*{{{ client */
static void query_done(httpc_t *c, const httpc_query_t *q, int status)
{
}
static void client_create(const char *s)
{
    httpc_cfg_t *cfg;
    sockunion_t su;

    if (_G.client) {
        obj_release(_G.client);
    }
    cfg = httpc_cfg_new();
    cfg->max_queries = 1000;
    my_addr_resolve(s, &su);
    _G.client = httpc_connect(&su, cfg, NULL);
    _G.client->on_query_done = &query_done;
    obj_retain(_G.client);
    httpc_cfg_release(&cfg);
}

static int query_on_hdrs(httpc_query_t *q) {
    return 0;
}
static void query_on_done(httpc_query_t *q, httpc_status_t status)
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
    compute_stats(&_G.stat, response_time);
    compute_stats(&_G.stat_global, response_time);
    httpc_query_wipe(q);
    p_delete(&q);
}

static void fire_one(httpc_t *c) {
    httpc_query_t *query = p_new(httpc_query_t, 1);
    httpc_query_init(query);
    httpc_bufferize(query, 40 << 20);
    query->on_done = &query_on_done;
    query->on_hdrs = &query_on_hdrs;

    httpc_query_attach(query, c);
    httpc_query_start(query, HTTP_METHOD_GET, _G.host, LSTR_IMMED_V("/"));
    httpc_query_hdrs_done(query, 0, false);
    httpc_query_done(query);
    query->pdata = (void *)lp_getmsec();
    _G.fire++;
}
static int fire(void)
{
    for (int i = 0; i < 100; i++) {
        if (!_G.client || !_G.client->ev || _G.client->max_queries <= 0) {
            client_create(_G.uri.s);
        }
        fire_one(_G.client);
    }
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

static void
_stats_print_headers(const char *first, const char *md,
                     const char *ms, const char *last)
{
    printf("%s═══════%s", first, md);
    printf("═══════%s", md);
    printf("═══════%s", md);
    printf("════%s", ms);
    printf(" 1s %s", ms);
    printf("════%s", md);
    printf("════%s", ms);
    printf(" All ");
    printf("════%s\n", last);
    printf("║  Fire ║   OK  ║ Error ");
    printf("║ Min│ Max│ Avg║ Min│ Max│ Avg║\n");
    printf("╟───────╫───────╫───────");
    printf("╫────┼────┼────╫────┼────┼────╢\n");
}
static void stats_print_first_header(void)
{
    _stats_print_headers("╔", "╦", "╤", "╗");
}
static void stats_print_header(void)
{
    _stats_print_headers("╠", "╬", "╪", "╣");
};
static void stats_print(stats_t *s)
{
    printf("║ %03ju",s->min);
    printf("│ %03ju",s->max);
    printf("│ %03ju",s->count ? s->sum / s->count : 0);
}
static void stats(el_t elh, data_t priv)
{
    static int nb = 0;
    if (nb++ > 5) {
        stats_print_header();
        nb = 0;
    }
    printf("║ %6d║ %6d║ %6d", _G.fire, _G.ok, _G.error);
    stats_print(&_G.stat);
    stats_print(&_G.stat_global);
    printf("║\n");
    stats_init(&_G.stat);
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
    stats_print_first_header();
    client_create(argv[0]);
    fire();
    el_signal_register(SIGTERM, &on_term, NULL);
    el_signal_register(SIGINT,  &on_term, NULL);
    el_signal_register(SIGQUIT, &on_term, NULL);
    el_timer_register(2000, 10, EL_TIMER_LOWRES, &slice_hook, NULL);
    el_timer_register(0, 1000, EL_TIMER_LOWRES, &stats, NULL);
    el_timer_register(10000, 0, EL_TIMER_LOWRES, &my_kill, NULL);


    /* got into event loop */
    el_loop();

    return 0;
}

/* }}} */
