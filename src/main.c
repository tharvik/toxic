/*
 * Toxic -- Tox Curses Client
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <curses.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <locale.h>
#include <string.h>
#include <netdb.h>

#ifdef _win32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#endif

#include <tox/tox.h>

#include "configdir.h"
#include "toxic_windows.h"
#include "prompt.h"
#include "friendlist.h"

#ifndef PACKAGE_DATADIR
#define PACKAGE_DATADIR "."
#endif
/* Export for use in Callbacks */
char *DATA_FILE = NULL;
char *SRVLIST_FILE = NULL;

void on_window_resize(int sig)
{
    endwin();
    refresh();
    clear();
}

static void init_term()
{
    /* Setup terminal */
    signal(SIGWINCH, on_window_resize);
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    keypad(stdscr, 1);
    noecho();
    timeout(100);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_CYAN, COLOR_BLACK);
        init_pair(3, COLOR_RED, COLOR_BLACK);
        init_pair(4, COLOR_BLUE, COLOR_BLACK);
        init_pair(5, COLOR_YELLOW, COLOR_BLACK);
        init_pair(6, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(7, COLOR_BLACK, COLOR_BLACK);
        init_pair(8, COLOR_BLACK, COLOR_WHITE);

    }

    refresh();
}

static Tox *init_tox()
{
    /* Init core */
    Tox *m = tox_new();

    /* Callbacks */
    tox_callback_friendrequest(m, on_request, NULL);
    tox_callback_friendmessage(m, on_message, NULL);
    tox_callback_namechange(m, on_nickchange, NULL);
    tox_callback_statusmessage(m, on_statuschange, NULL);
    tox_callback_action(m, on_action, NULL);
#ifdef __linux__
    tox_setname(m, (uint8_t *) "Cool guy", sizeof("Cool guy"));
#elif defined(WIN32)
    tox_setname(m, (uint8_t *) "I should install GNU/Linux", sizeof("I should install GNU/Linux"));
#elif defined(__APPLE__)
    tox_setname(m, (uint8_t *) "Hipster", sizeof("Hipster")); //This used to users of other Unixes are hipsters
#else
    tox_setname(m, (uint8_t *) "Registered Minix user #4", sizeof("Registered Minix user #4"));
#endif
    return m;
}

/*
  resolve_addr():
    address should represent IPv4 or a hostname with A record

    returns a data in network byte order that can be used to set IP.i or IP_Port.ip.i
    returns 0 on failure

    TODO: Fix ipv6 support
*/
uint32_t resolve_addr(const char *address)
{
    struct addrinfo *server = NULL;
    struct addrinfo  hints;
    int              rc;
    uint32_t         addr;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;    // IPv4 only right now.
    hints.ai_socktype = SOCK_DGRAM; // type of socket Tox uses.

    rc = getaddrinfo(address, "echo", &hints, &server);

    // Lookup failed.
    if (rc != 0) {
        return 0;
    }

    // IPv4 records only..
    if (server->ai_family != AF_INET) {
        freeaddrinfo(server);
        return 0;
    }


    addr = ((struct sockaddr_in *)server->ai_addr)->sin_addr.s_addr;

    freeaddrinfo(server);
    return addr;
}

#define MAXLINE 90    /* Approx max number of chars in a sever line (IP + port + key) */
#define MINLINE 70
#define MAXSERVERS 50

/* Connects to a random DHT server listed in the DHTservers file */
int init_connection(Tox *m)
{
    FILE *fp = NULL;

    if (tox_isconnected(m))
        return 0;

    fp = fopen(SRVLIST_FILE, "r");

    if (!fp)
        return 1;

    char servers[MAXSERVERS][MAXLINE];
    char line[MAXLINE];
    int linecnt = 0;

    while (fgets(line, sizeof(line), fp) && linecnt < MAXSERVERS) {
        if (strlen(line) > MINLINE)
            strcpy(servers[linecnt++], line);
    }

    if (linecnt < 1) {
        fclose(fp);
        return 2;
    }

    fclose(fp);

    char *server = servers[rand() % linecnt];
    char *ip = strtok(server, " ");
    char *port = strtok(NULL, " ");
    char *key = strtok(NULL, " ");

    if (!ip || !port || !key)
        return 3;

    tox_IP_Port dht;
    dht.port = htons(atoi(port));
    uint32_t resolved_address = resolve_addr(ip);

    if (resolved_address == 0)
        return 0;

    dht.ip.i = resolved_address;
    uint8_t *binary_string = hex_string_to_bin(key);
    tox_bootstrap(m, dht, binary_string);
    free(binary_string);
    return 0;
}

static void do_tox(Tox *m, ToxWindow *prompt)
{
    static int conn_try = 0;
    static int conn_err = 0;
    static bool dht_on = false;

    if (!dht_on && !tox_isconnected(m) && !(conn_try++ % 100)) {
        if (!conn_err) {
            conn_err = init_connection(m);
            wprintw(prompt->window, "\nEstablishing connection...\n");

            if (conn_err)
                wprintw(prompt->window, "\nAuto-connect failed with error code %d\n", conn_err);
        }
    } else if (!dht_on && tox_isconnected(m)) {
        dht_on = true;
        wprintw(prompt->window, "\nDHT connected.\n");
    } else if (dht_on && !tox_isconnected(m)) {
        dht_on = false;
        wprintw(prompt->window, "\nDHT disconnected. Attempting to reconnect.\n");
    }

    tox_do(m);
}

int f_loadfromfile;

/*
 * Store Messenger to given location
 * Return 0 stored successfully
 * Return 1 malloc failed
 * Return 2 opening path failed
 * Return 3 fwrite failed
 */
int store_data(Tox *m, char *path)
{
    if (f_loadfromfile == 0) /*If file loading/saving is disabled*/
        return 0;

    FILE *fd;
    size_t len;
    uint8_t *buf;

    len = tox_size(m);
    buf = malloc(len);

    if (buf == NULL) {
        return 1;
    }

    tox_save(m, buf);

    fd = fopen(path, "w");

    if (fd == NULL) {
        free(buf);
        return 2;
    }

    if (fwrite(buf, len, 1, fd) != 1) {
        free(buf);
        fclose(fd);
        return 3;
    }

    free(buf);
    fclose(fd);
    return 0;
}

static void load_data(Tox *m, char *path)
{
    if (f_loadfromfile == 0) /*If file loading/saving is disabled*/
        return;

    FILE *fd;
    size_t len;
    uint8_t *buf;

    if ((fd = fopen(path, "r")) != NULL) {
        fseek(fd, 0, SEEK_END);
        len = ftell(fd);
        fseek(fd, 0, SEEK_SET);

        buf = malloc(len);

        if (buf == NULL) {
            fprintf(stderr, "malloc() failed.\n");
            fclose(fd);
            endwin();
            exit(1);
        }

        if (fread(buf, len, 1, fd) != 1) {
            fprintf(stderr, "fread() failed.\n");
            free(buf);
            fclose(fd);
            endwin();
            exit(1);
        }

        tox_load(m, buf, len);

        uint32_t i = 0;

        char name[TOX_MAX_NAME_LENGTH];
        while (tox_getname(m, i, (uint8_t *)name) != -1) {
            on_friendadded(m, i);
            i++;
        }

        free(buf);
        fclose(fd);
    } else {
        int st;

        if ((st = store_data(m, path)) != 0) {
            fprintf(stderr, "Store messenger failed with return code: %d\n", st);
            endwin();
            exit(1);
        }
    }
}

int main(int argc, char *argv[])
{
    char *user_config_dir = get_user_config_dir();
    int config_err = 0;

    f_loadfromfile = 1;
    int f_flag = 0;
    int i = 0;

    for (i = 0; i < argc; ++i) {
        if (argv[i] == NULL)
            break;
        else if (argv[i][0] == '-') {
            if (argv[i][1] == 'f') {
                if (argv[i + 1] != NULL)
                    DATA_FILE = strdup(argv[i + 1]);
                else
                    f_flag = -1;
            } else if (argv[i][1] == 'n') {
                f_loadfromfile = 0;
            }
        }
    }

    if (DATA_FILE == NULL ) {
        config_err = create_user_config_dir(user_config_dir);

        if (config_err) {
            DATA_FILE = strdup("data");
            SRVLIST_FILE = strdup(PACKAGE_DATADIR "/DHTservers");
        } else {
            DATA_FILE = malloc(strlen(user_config_dir) + strlen(CONFIGDIR) + strlen("data") + 1);
            strcpy(DATA_FILE, user_config_dir);
            strcat(DATA_FILE, CONFIGDIR);
            strcat(DATA_FILE, "data");

            SRVLIST_FILE = malloc(strlen(user_config_dir) + strlen(CONFIGDIR) + strlen("DHTservers") + 1);
            strcpy(SRVLIST_FILE, user_config_dir);
            strcat(SRVLIST_FILE, CONFIGDIR);
            strcat(SRVLIST_FILE, "DHTservers");
        }
    }

    free(user_config_dir);

    init_term();
    Tox *m = init_tox();
    ToxWindow *prompt = init_windows(m);

    if (f_loadfromfile)
        load_data(m, DATA_FILE);

    if (f_flag == -1) {
        attron(COLOR_PAIR(3) | A_BOLD);
        wprintw(prompt->window, "You passed '-f' without giving an argument.\n"
                "defaulting to 'data' for a keyfile...\n");
        attroff(COLOR_PAIR(3) | A_BOLD);
    }

    if (config_err) {
        attron(COLOR_PAIR(3) | A_BOLD);
        wprintw(prompt->window, "Unable to determine configuration directory.\n"
                "defaulting to 'data' for a keyfile...\n");
        attroff(COLOR_PAIR(3) | A_BOLD);
    }

    while (true) {
        /* Update tox */
        do_tox(m, prompt);

        /* Draw */
        draw_active_window(m);
    }

    tox_kill(m);
    free(DATA_FILE);
    free(SRVLIST_FILE);
    return 0;
}
