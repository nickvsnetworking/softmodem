/*
 * softmodem configuration: parsed from a simple INI file and/or CLI flags.
 */
#ifndef SOFTMODEM_CONFIG_H_
#define SOFTMODEM_CONFIG_H_

typedef enum {
    SIP_MODE_DIRECT = 0,   /* peer-to-peer INVITE to a known IP   */
    SIP_MODE_REGISTRAR     /* REGISTER to a proxy, route via it   */
} sip_mode_t;

typedef struct {
    /* --- SIP --- */
    sip_mode_t  sip_mode;
    char        sip_bind[64];      /* "0.0.0.0:5060"                 */
    char        sip_peer[128];     /* ATA IP[:port] or INVITE target */
    char        sip_registrar[128];
    char        sip_user[64];
    char        sip_pass[64];

    /* --- media --- */
    char        codecs[64];        /* "PCMA,PCMU"                    */
    int         ptime_ms;          /* 20                             */
    int         echo_can;          /* bool (kept OFF for modems)     */
    char        media_ip[64];      /* local RTP IP advertised in SDP */
    int         media_port;        /* local RTP base port            */

    /* --- modem --- */
    char        modem_mode[16];    /* "B212"                         */

    /* --- virtual tty --- */
    char        tty_device[128];   /* symlink path, e.g. /dev/ttyMODEM0 */

    int         log_level;         /* 0=err 1=info 2=debug          */
} softmodem_config_t;

void config_defaults(softmodem_config_t *cfg);
int  config_load_file(softmodem_config_t *cfg, const char *path);
/* Returns 0 on success, <0 on parse/usage error, >0 if --help was requested. */
int  config_parse_args(softmodem_config_t *cfg, int argc, char **argv);
void config_dump(const softmodem_config_t *cfg);

#endif /* SOFTMODEM_CONFIG_H_ */
