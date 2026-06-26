#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

void config_defaults(softmodem_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->sip_mode = SIP_MODE_DIRECT;
    snprintf(cfg->sip_bind,   sizeof(cfg->sip_bind),   "0.0.0.0:5060");
    snprintf(cfg->codecs,     sizeof(cfg->codecs),     "PCMA,PCMU");
    cfg->ptime_ms = 20;
    /* OFF by default and intentionally so: Bell 212A / V.22 is full-duplex by
     * frequency-division (originate = low band, answer = high band), and each
     * receiver's band filter rejects its own near-end echo. A voice echo
     * canceller would corrupt the modem tones, so we don't cancel. The ATA and
     * any PSTN/SIP path must likewise have EC/VAD/CNG disabled. */
    cfg->echo_can = 0;
    snprintf(cfg->modem_mode, sizeof(cfg->modem_mode), "B212");
    snprintf(cfg->tty_device, sizeof(cfg->tty_device), "/dev/ttyMODEM0");
    cfg->log_level = 1;
}

/* Trim leading/trailing whitespace in place; returns pointer into s. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static void set_kv(softmodem_config_t *cfg, const char *key, const char *val) {
    if      (!strcmp(key, "mode"))      cfg->sip_mode = !strcmp(val, "registrar")
                                                        ? SIP_MODE_REGISTRAR : SIP_MODE_DIRECT;
    else if (!strcmp(key, "bind"))      snprintf(cfg->sip_bind,      sizeof(cfg->sip_bind),      "%s", val);
    else if (!strcmp(key, "peer"))      snprintf(cfg->sip_peer,      sizeof(cfg->sip_peer),      "%s", val);
    else if (!strcmp(key, "registrar")) snprintf(cfg->sip_registrar, sizeof(cfg->sip_registrar), "%s", val);
    else if (!strcmp(key, "user"))      snprintf(cfg->sip_user,      sizeof(cfg->sip_user),      "%s", val);
    else if (!strcmp(key, "pass"))      snprintf(cfg->sip_pass,      sizeof(cfg->sip_pass),      "%s", val);
    else if (!strcmp(key, "codecs"))    snprintf(cfg->codecs,        sizeof(cfg->codecs),        "%s", val);
    else if (!strcmp(key, "ptime"))     cfg->ptime_ms = atoi(val);
    else if (!strcmp(key, "echo_can"))  cfg->echo_can = (!strcmp(val, "on") || !strcmp(val, "1"));
    else if (!strcmp(key, "modem"))     snprintf(cfg->modem_mode,    sizeof(cfg->modem_mode),    "%s", val);
    else if (!strcmp(key, "device"))    snprintf(cfg->tty_device,    sizeof(cfg->tty_device),    "%s", val);
    else fprintf(stderr, "config: ignoring unknown key '%s'\n", key);
}

int config_load_file(softmodem_config_t *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config: cannot open %s\n", path);
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';' || *s == '[') continue; /* skip blanks/comments/sections */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        set_kv(cfg, trim(s), trim(eq + 1));
    }
    fclose(f);
    return 0;
}

static void usage(const char *prog) {
    printf(
        "Usage: %s [options]\n"
        "  -c, --config <file>     Load INI config file\n"
        "  -d, --device <path>     Virtual tty symlink (default /dev/ttyMODEM0)\n"
        "  -m, --mode <m>          SIP mode: direct | registrar\n"
        "  -b, --bind <ip:port>    SIP bind address (default 0.0.0.0:5060)\n"
        "  -p, --peer <ip[:port]>  ATA / peer address (direct mode)\n"
        "  -r, --registrar <host>  Registrar (registrar mode)\n"
        "  -u, --user <user>       SIP username\n"
        "  -w, --pass <pass>       SIP password\n"
        "  -e, --echo-can <on|off> Echo canceller (default OFF; keep off for modems)\n"
        "  -v, --verbose           Increase log level (repeatable)\n"
        "  -h, --help              This help\n",
        prog);
}

int config_parse_args(softmodem_config_t *cfg, int argc, char **argv) {
    static const struct option opts[] = {
        {"config",    required_argument, 0, 'c'},
        {"device",    required_argument, 0, 'd'},
        {"mode",      required_argument, 0, 'm'},
        {"bind",      required_argument, 0, 'b'},
        {"peer",      required_argument, 0, 'p'},
        {"registrar", required_argument, 0, 'r'},
        {"user",      required_argument, 0, 'u'},
        {"pass",      required_argument, 0, 'w'},
        {"echo-can",  required_argument, 0, 'e'},
        {"verbose",   no_argument,       0, 'v'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    /* First pass: honour --config so CLI flags can override file values. */
    optind = 1;
    while ((c = getopt_long(argc, argv, "c:d:m:b:p:r:u:w:e:vh", opts, NULL)) != -1) {
        if (c == 'c') config_load_file(cfg, optarg);
    }
    optind = 1;
    while ((c = getopt_long(argc, argv, "c:d:m:b:p:r:u:w:e:vh", opts, NULL)) != -1) {
        switch (c) {
        case 'c': break; /* already handled */
        case 'd': set_kv(cfg, "device", optarg); break;
        case 'm': set_kv(cfg, "mode", optarg); break;
        case 'b': set_kv(cfg, "bind", optarg); break;
        case 'p': set_kv(cfg, "peer", optarg); break;
        case 'r': set_kv(cfg, "registrar", optarg); break;
        case 'u': set_kv(cfg, "user", optarg); break;
        case 'w': set_kv(cfg, "pass", optarg); break;
        case 'e': set_kv(cfg, "echo_can", optarg); break;
        case 'v': cfg->log_level++; break;
        case 'h': usage(argv[0]); return 1;
        default:  usage(argv[0]); return -1;
        }
    }
    return 0;
}

void config_dump(const softmodem_config_t *cfg) {
    printf("softmodem config:\n");
    printf("  sip.mode      = %s\n", cfg->sip_mode == SIP_MODE_REGISTRAR ? "registrar" : "direct");
    printf("  sip.bind      = %s\n", cfg->sip_bind);
    printf("  sip.peer      = %s\n", cfg->sip_peer[0] ? cfg->sip_peer : "(unset)");
    printf("  sip.registrar = %s\n", cfg->sip_registrar[0] ? cfg->sip_registrar : "(unset)");
    printf("  sip.user      = %s\n", cfg->sip_user[0] ? cfg->sip_user : "(unset)");
    printf("  media.codecs  = %s  ptime=%dms  echo_can=%s\n",
           cfg->codecs, cfg->ptime_ms, cfg->echo_can ? "on" : "off");
    printf("  modem.mode    = %s\n", cfg->modem_mode);
    printf("  tty.device    = %s\n", cfg->tty_device);
}
