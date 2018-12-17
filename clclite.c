/**
 * Command-Line Client
 * Shane Feek and Sean Middleditch
 * THIS CODE IS PUBLIC DOMAIN
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <arpa/telnet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <ncurses.h>

#ifdef HAVE_ZLIB
# include <zlib.h>
#endif

#include "libtelnet.h"

/* telnet protocol */
static telnet_t *telnet;

static const telnet_telopt_t telnet_telopts[] = {
	{ TELNET_TELOPT_ECHO, 		TELNET_WONT, TELNET_DO   },
	{ TELNET_TELOPT_NAWS, 		TELNET_WILL, TELNET_DONT },
	{ TELNET_TELOPT_COMPRESS2,	TELNET_WONT, TELNET_DO   },
	{ TELNET_TELOPT_ZMP, 		TELNET_WONT, TELNET_DO   },
	{ -1, 0, 0 }
};

/* telnet handler functions */
static void telnet_event(telnet_t* telnet, telnet_event_t *event, void*);
static void send_line(const char* line, size_t len);
static void send_naws(void);

/* terminal processing */
typedef enum { TERM_ASCII, TERM_ESC, TERM_ESCRUN } term_state_t;

#define TERM_MAX_ESC 16
#define TERM_COLOR_DEFAULT 9

#define TERM_FLAG_ECHO (1<<0)
#define TERM_FLAG_NAWS (1<<2)
#define TERM_FLAGS_DEFAULT (TERM_FLAG_ECHO)

static struct TERMINAL 
{
	term_state_t state;
	int esc_buf[TERM_MAX_ESC];
	size_t esc_cnt;
	char flags;
	int color;
} terminal;

/* edit buffer */
#define EDITBUF_MAX 1001

static struct EDITBUF 
{
	char buf[EDITBUF_MAX];
	size_t size;
	size_t pos;
	size_t start;
} editbuf;

static void editbuf_set(const char*);
static void editbuf_insert(int);
static void editbuf_bs();
static void editbuf_del();
static void editbuf_curleft();
static void editbuf_curright();
static void editbuf_display();
static void editbuf_home();
static void editbuf_end();

/* screen buffer */
#define MAXLINES 100
#define MAX_LINE_LENGTH 1001

char sbuffer[MAXLINES][MAX_LINE_LENGTH];

static int last=0;
static int windowpos=0;
static int updowntoggle;

/* running flag; when 0, exit main loop */
static int running = 1;

/* banner buffer */
static char banner[1001];
static int autobanner = 1;

/* windows */
static WINDOW* win_main = 0;
static WINDOW* win_input = 0;
static WINDOW* win_banner = 0;

/* last interrupt */
volatile int have_sigwinch = 0;
volatile int have_sigint = 0;

/* server socket */
static char* host = "localhost";
static char* port = "6969";
static int sock;
static size_t sent_bytes = 0;
static size_t recv_bytes = 0;

/* core functions */
static void on_text_plain (const char* text, size_t len);
static void on_text_ansi (const char* text, size_t len);
static void send_text_ansi (const char*, size_t);

/* ======= CORE ======= */

/* Initialize the buffer */
void init_buffer(char buffer[][MAX_LINE_LENGTH])
{
    int y;
    for(y=0; y<MAXLINES; y++) buffer[y][0]='\0';
}

/* Scroll the buffer */
void scroll_buffer(char buffer[][MAX_LINE_LENGTH])
{
    int y;
    for(y=1; y<MAXLINES; y++) strcpy(buffer[y-1],buffer[y]);
    buffer[y-1][0]='\0';
}

/* Add entire line to the buffer */
int add_line_buffer(char buffer[][MAX_LINE_LENGTH], char *line, int lastline)
{
    if (lastline == MAXLINES)
    {
        scroll_buffer(buffer);
        lastline--;
    }

    if (strlen(line) > (MAX_LINE_LENGTH - 2)) line[MAX_LINE_LENGTH - 2] = '\0'; /* Don't go off the end! */

    strcpy(buffer[lastline],line);
    lastline++;

    return lastline;
} 

/* Add characters to the buffer */
int add_char_buffer(char buffer[][MAX_LINE_LENGTH], char c, int lastline)
{
    int x;

    if (lastline == MAXLINES)
    {
        scroll_buffer(buffer);
        lastline--;
    }

    for(x=0; buffer[lastline][x]; x++); /* find the end character of the current line */

    if (x > (MAX_LINE_LENGTH - 2)) x = MAX_LINE_LENGTH - 2; /* Don't go off the end! */
    
    buffer[lastline][x]=c;
    buffer[lastline][x+1]='\0'; /* make it a string */
  
    if (c == '\n')  lastline++;

    return lastline;
}

/* Print buffer to the screen */
int print_buffer(char buffer[][MAX_LINE_LENGTH], int startline, int endline)
{
	wclear(win_main); 

    if (startline < 0) startline = 0;
    if (startline > endline) return 1;
    if (endline > MAXLINES) endline = MAXLINES;

    int y;
    for(y=startline; y<=endline; y++) 
	{
		send_text_ansi(buffer[y],strlen(buffer[y]));
	}

	updowntoggle = 0;

	return 0;
}

/* cleanup function */
static void cleanup (void) 
{
	/* cleanup curses */
	endwin();
}

/* handle signals */
static void handle_signal (int sig) 
{
	switch (sig) 
	{
		case SIGWINCH:
			have_sigwinch = 1;
			break;
		case SIGINT:
			have_sigint = 1;
			break;
	}
}

/* set the edit buffer to contain the given text */
static void editbuf_set (const char* text) 
{
	snprintf(editbuf.buf, EDITBUF_MAX, "%s", text);
	editbuf.pos = editbuf.size = strlen(text);
}

/* insert/replace a character at the current location */
static void editbuf_insert (int ch) 
{
	/* ensure we have space */
	if (editbuf.size == EDITBUF_MAX)
		return;

	/* if we're at the end, just append the character */
	if (editbuf.pos == editbuf.size) 
	{
		editbuf.buf[editbuf.pos] = ch;
		editbuf.pos = ++editbuf.size;
		return;
	}

	/* move data, insert character */
	memmove(editbuf.buf + editbuf.pos + 1, editbuf.buf + editbuf.pos, editbuf.size - editbuf.pos);
	editbuf.buf[editbuf.pos] = ch;
	++editbuf.pos;
	++editbuf.size;
}

/* delete character one position to the left */
static void editbuf_bs () 
{
	/* if we're at the beginning, do nothing */
	if (editbuf.pos == 0)
		return;

	/* if we're at the end, just decrement pos and size */
	if (editbuf.pos == editbuf.size) 
	{
		editbuf.pos = --editbuf.size;
		return;
	}

	/* chop out the previous character */
	memmove(editbuf.buf + editbuf.pos - 1, editbuf.buf + editbuf.pos, editbuf.size - editbuf.pos);
	--editbuf.pos;
	--editbuf.size;
}

/* delete letter under cursor */
static void editbuf_del () 
{
	/* if we're at the end, do nothing */
	if (editbuf.pos == editbuf.size)
		return;

	/* if we're at the end, just decrement pos and size */
	if (editbuf.pos == editbuf.size - 1) 
	{
		--editbuf.pos;
		--editbuf.size;
		return;
	}

	/* chop out the current character */
	memmove(editbuf.buf + editbuf.pos, editbuf.buf + editbuf.pos + 1, editbuf.size - editbuf.pos - 1);
	--editbuf.size;
}

/* move to home position */
static void editbuf_home () 
{
	editbuf.pos = 0;
	editbuf.start = 0;
}

/* move to end position */
static void editbuf_end () 
{
	editbuf.pos = editbuf.size;
}

/* move cursor left */
static void editbuf_curleft () 
{
	if (editbuf.pos > 0) --editbuf.pos;
}

/* move cursor right */
static void editbuf_curright () 
{
	if (editbuf.pos < editbuf.size)	++editbuf.pos;
}

/* display the edit buffer in win_input */
static void editbuf_display () 
{
	if (editbuf.pos >= COLS)
		editbuf.start = editbuf.pos - COLS;
	else
		editbuf.start = 0;

	wclear(win_input);
	mvwaddnstr(win_input, 0, 0, editbuf.buf + editbuf.start, editbuf.size - editbuf.start);
	wmove(win_input, 0, editbuf.pos);
}

/* paint banner */
static void paint_banner (void) 
{
	/* if autobanner is on, build our banner buffer */
	if (autobanner) 
	{
		snprintf(banner, sizeof(banner), "%s:%s - (%s)", host, port, sock == -1 ? "disconnected" : "connected");
	}

	wclear(win_banner);
	mvwaddstr(win_banner, 0, 0, banner);
}

/* redraw all windows */
static void redraw_display (void) 
{
	/* get size */
	struct winsize ws;
	if (ioctl(0, TIOCGWINSZ, &ws))
		return;

	/* resize */
	resizeterm(ws.ws_row, ws.ws_col);
	mvwin(win_input, LINES-1, 0);
	wresize(win_input, 1, COLS);
	mvwin(win_banner, LINES-2, 0);
	wresize(win_banner, 1, COLS);
	wresize(win_main, LINES-2, COLS);

	/* update */
	paint_banner();

	/* update size */
	if (running) send_naws();

	/* input display */
	editbuf_display();

	/* refresh */
	if (!updowntoggle)
	{
		windowpos = last - LINES + 2;
		if (windowpos < 0) windowpos = 0;
	}

	print_buffer(sbuffer,windowpos,windowpos + LINES - 3);
	
	wnoutrefresh(win_main);
	wnoutrefresh(win_banner);
	wnoutrefresh(win_input);
	doupdate();
}

/* force-send bytes to server */
static void do_send (const char* bytes, size_t len) 
{
	int ret;

	/* keep sending bytes until they're all sent */
	while (len > 0) 
	{
		ret = send(sock, bytes, len, 0);
	
		if (ret == -1) 
		{
			if (ret != EAGAIN && ret != EINTR) 
			{
				endwin();
				fprintf(stderr, "send() failed: %s\n", strerror(errno));
				exit(1);
			}
			continue;
		} 
		else if (ret == 0) 
		{
			endwin();
			printf("Disconnected from server\n");
			exit(0);
		}
		else
		{
			sent_bytes += ret;
			bytes += ret;
			len -= ret;
		}
	}
}

/* process user input */
static int on_key (int key) 
{
	int full_refresh = 0;

	/* special keys */
	if (key >= KEY_MIN && key <= KEY_MAX) 
	{
		switch (key)
		{
			case KEY_ENTER: /* send */
				send_line(editbuf.buf, editbuf.size);
				editbuf_set("");
				full_refresh = 1;
				break;

			case KEY_BACKSPACE: 
				editbuf_bs();
				break;
		
			case KEY_DC:
				editbuf_del();
				break;
		
			case KEY_LEFT:
				editbuf_curleft();
				break;

			case KEY_RIGHT:
				editbuf_curright();
				break;

			case KEY_HOME:
				editbuf_home();
				break;
		
			case KEY_END:
				editbuf_end();
				break;

			case KEY_DOWN:
				updowntoggle = 1;
				windowpos++;
				if (windowpos > MAXLINES ) windowpos = MAXLINES;
				full_refresh = 1;
				break;

			case KEY_NPAGE:
				updowntoggle = 1;
				windowpos+=10;
				if (windowpos > MAXLINES ) windowpos = MAXLINES;
				full_refresh = 1;
				break;

			case KEY_UP:
				updowntoggle = 1;
				windowpos--;
				if (windowpos < 0) windowpos = 0;
				full_refresh = 1;
				break;

			case KEY_PPAGE:
				updowntoggle = 1;
				windowpos-=10;
				if (windowpos < 0) windowpos = 0;
				full_refresh = 1;
				break;
		}
	} 
	else 
	{
		/* send */
		if (key == '\n' || key == '\r') 
		{
			send_line(editbuf.buf, editbuf.size);
			editbuf_set("");
		} 
		else 
		{
			editbuf_insert(key);
		}
	}

	/* draw input */
	editbuf_display();
	return full_refresh;
}

/* perform a terminal escape */
static void on_term_esc(char cmd) 
{
	size_t i;

	if (cmd == 'm')
	{
		for (i = 0; i < terminal.esc_cnt; ++i) 
		{
			/* default */
			if (terminal.esc_buf[i] == 0) 
			{
				terminal.color = TERM_COLOR_DEFAULT;
				wattron(win_main, COLOR_PAIR(terminal.color));
			}
			/* color */
			else if (terminal.esc_buf[i] >= 31 && terminal.esc_buf[i] <= 37) 
			{
				terminal.color = terminal.esc_buf[i] - 30;
				wattron(win_main, COLOR_PAIR(terminal.color));
			}
		}
	}
}

/* process text into virtual terminal, no ANSI */
static void on_text_plain (const char* text, size_t len) 
{
	size_t i;
	for (i = 0; i < len; ++i) last = add_char_buffer(sbuffer,text[i], last);
}

/* process text into virtual terminal */
static void send_text_ansi (const char* text, size_t len) 
{
	size_t i;
	for (i = 0; i < len; ++i) {
		switch (terminal.state) {
			case TERM_ASCII:
				/* begin escape sequence */
				if (text[i] == 27)
				{
					terminal.state = TERM_ESC;
				}
				/* just show it */
				else if (text[i] != '\r')
				{
					waddch(win_main, text[i]);
				}
				break;
			case TERM_ESC:
				/* run of mod setting commands */
				if (text[i] == '[') 
				{
					terminal.state = TERM_ESCRUN;
					terminal.esc_cnt = 0;
					terminal.esc_buf[0] = 0;
				}
				/* something else we don't support */
				else
					terminal.state = TERM_ASCII;
				break;
			case TERM_ESCRUN:
				/* number, add to option */
				if (isdigit(text[i])) 
				{
					if (terminal.esc_cnt == 0)
						terminal.esc_cnt = 1;
					terminal.esc_buf[terminal.esc_cnt-1] *= 10;
					terminal.esc_buf[terminal.esc_cnt-1] += text[i] - '0';
				}
				/* semi-colon, go to next option */
				else if (text[i] == ';') 
				{
					if (terminal.esc_cnt < TERM_MAX_ESC) 
					{
						terminal.esc_cnt++;
						terminal.esc_buf[terminal.esc_cnt-1] = 0;
					}
				}
				/* anything-else; perform option */
				else {
					on_term_esc(text[i]);
					terminal.state = TERM_ASCII;
				}
				break;
		}
	}
}

/* send text to virtual terminal */
static void on_text_ansi (const char* text, size_t len) 
{
	size_t i;
	for (i = 0; i < len; ++i)
	{
		if (text[i]=='\007') { beep(); continue; } /* If we have an incoming bell, play it and skip it*/
		last = add_char_buffer(sbuffer,text[i], last);
	}
}

/* attempt to connect to the requested hostname on the request port */
static int do_connect (const char* host, const char* port) 
{
	struct addrinfo hints;
	struct addrinfo *results;
	struct addrinfo *ai;
	int ret;
	int sock;

	/* lookup host */
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((ret = getaddrinfo(host, port, &hints, &results)) != 0) 
	{
		fprintf(stderr, "Host lookup failed: %s\n", gai_strerror(ret));
		return -1;
	}

	/* loop through hosts, trying to connect */
	for (ai = results; ai != NULL; ai = ai->ai_next) 
	{
		/* create socket */
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock == -1) 
		{
			fprintf(stderr, "socket() failed: %s\n", strerror(errno));
			exit(1);
		}

		/* connect */
		if (connect(sock, ai->ai_addr, ai->ai_addrlen) != -1) 
		{
			freeaddrinfo(results);
			return sock;
		}

		/* release socket */
		shutdown(sock, SHUT_RDWR);
	}

	/* could not connect */
	freeaddrinfo(results);
	return -1;
}

/* ============================ MAIN ================================*/
int main (int argc, char** argv) 
{
	struct sigaction sa;

	/* cleanup on any failure */
	atexit(cleanup);

	/* set terminal defaults */
	memset(&terminal, 0, sizeof(struct TERMINAL));
	terminal.state = TERM_ASCII;
	terminal.flags = TERM_FLAGS_DEFAULT;
	terminal.color = TERM_COLOR_DEFAULT;

	/* initialize telnet handler */
	telnet = telnet_init(telnet_telopts, telnet_event, 0, 0);

	/* Check for arguments */
	if (argc == 3)
	{
		host=argv[1];
		port=argv[2];
	}

	/* connect to server */
	sock = do_connect(host, port);
	if (sock == -1) 
	{
		fprintf(stderr, "Failed to connect to %s:%s\n", host, port);
		exit(1);
	}
	printf("Connected to %s:%s\n", host, port);

	/* intialize screen buffer */
	init_buffer(sbuffer);

	/* set initial banner */
	snprintf(banner, sizeof(banner), "CLC - %s:%s (connected)", host, port);

	/* configure curses */
	initscr();
	start_color();
	nonl();
	cbreak();
	noecho();

	win_main = newwin(LINES-2, COLS, 0, 0); 
	win_banner = newwin(1, COLS, LINES-2, 0);
	win_input = newwin(1, COLS, LINES-1, 0);

	idlok(win_main, TRUE);
	scrollok(win_main, TRUE);

	nodelay(win_input, FALSE);
	keypad(win_input, TRUE);

	use_default_colors();

	init_pair(COLOR_RED, COLOR_RED, -1);
	init_pair(COLOR_BLUE, COLOR_BLUE, -1);
	init_pair(COLOR_GREEN, COLOR_GREEN, -1);
	init_pair(COLOR_CYAN, COLOR_CYAN, -1);
	init_pair(COLOR_MAGENTA, COLOR_MAGENTA, -1);
	init_pair(COLOR_YELLOW, COLOR_YELLOW, -1);
	init_pair(COLOR_WHITE, COLOR_WHITE, -1);

	init_pair(TERM_COLOR_DEFAULT, -1, -1);
	wbkgd(win_main, COLOR_PAIR(TERM_COLOR_DEFAULT));
	wclear(win_main);
	init_pair(10, COLOR_WHITE, COLOR_BLUE);
	wbkgd(win_banner, COLOR_PAIR(10));
	wclear(win_banner);
	init_pair(11, -1, -1);
	wbkgd(win_input, COLOR_PAIR(11));
	wclear(win_input);

	/* clear and get screen started for the first time */
	redraw_display();

	/* set signal handlers */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGWINCH, &sa, NULL);

	/* initial edit buffer */
	memset(&editbuf, 0, sizeof(struct EDITBUF));

	/* setup poll info */
	struct pollfd fds[2];
	fds[0].fd = 1;
	fds[0].events = POLLIN;
	fds[1].fd = sock;
	fds[1].events = POLLIN;

	/* main loop */
	while (running) 
	{
		/* poll sockets */
		if (poll(fds, 2, -1) == -1) 
		{
			if (errno != EAGAIN && errno != EINTR) 
			{
				endwin();
				fprintf(stderr, "poll() failed: %s\n", strerror(errno));
				return 1;
			}
		}

		/* resize event? */
		if (have_sigwinch) 
		{
			have_sigwinch = 0;
			redraw_display();
		}

		/* escape? */
		if (have_sigint) 
		{
			exit(0);
		}

		/* input? */
		if (fds[0].revents & POLLIN) 
		{
			int key = wgetch(win_input);
			if (key != ERR)
			{
				if (on_key(key)==0)
				{
					wnoutrefresh(win_input);
					doupdate();
					continue;
				}
			}
		}

		/* process input data */
		if (fds[1].revents & POLLIN) 
		{
			char buffer[2048];
			int ret = recv(sock, buffer, sizeof(buffer), 0);
			if (ret == -1) 
			{
				if (errno != EAGAIN && errno != EINTR) 
				{
					endwin();
					fprintf(stderr, "recv() failed: %s\n", strerror(errno));
					return 1;
				}
			} 
			else if (ret == 0) 
			{
				running = 0;
			} 
			else 
			{
				recv_bytes += ret;
				telnet_recv(telnet, buffer, ret);
			}
		}

		/* Did we just scroll manually? */
		if (!updowntoggle)
		{
			windowpos = last - LINES + 2;
			if (windowpos < 0) windowpos = 0;
		}

		/* flush output */
		paint_banner();
		print_buffer(sbuffer,windowpos,windowpos + LINES - 3);
		wnoutrefresh(win_main);
		wnoutrefresh(win_banner);
		wnoutrefresh(win_input);
		doupdate();
	}

	/* final display, pause */
	sock = -1;
	autobanner = 1;
	paint_banner();
	wnoutrefresh(win_banner);
	doupdate();
	wgetch(win_input);

	/* clean up */
	endwin();
	printf("Disconnected.\n");

	/* free memory (so Valgrind leak detection is useful) */
	telnet_free(telnet);

	return 0;
}

/* ======= TELNET ======= */

/* telnet event handler */
static void telnet_event (telnet_t* telnet, telnet_event_t* ev, void* ud) 
{
	switch (ev->type) 
	{
		case TELNET_EV_DATA:
			on_text_ansi(ev->data.buffer, ev->data.size);
			break;
	
		case TELNET_EV_SEND:
			do_send(ev->data.buffer, ev->data.size);
			break;
	
		case TELNET_EV_WILL:
			if (ev->neg.telopt == TELNET_TELOPT_ECHO)
				terminal.flags &= ~TERM_FLAG_ECHO;
			break;
	
		case TELNET_EV_WONT:
			if (ev->neg.telopt == TELNET_TELOPT_ECHO)
				terminal.flags |= TERM_FLAG_ECHO;
			break;

		case TELNET_EV_DO:
			if (ev->neg.telopt == TELNET_TELOPT_NAWS) 
			{
				terminal.flags |= TERM_FLAG_NAWS;
				send_naws();
			}
			break;

		case TELNET_EV_WARNING:
			wattron(win_main, COLOR_PAIR(COLOR_RED));
			on_text_plain("\nWARNING:", 8);
			on_text_plain(ev->error.msg, strlen(ev->error.msg));
			on_text_plain("\n", 1);
			wattron(win_main, COLOR_PAIR(terminal.color));
			break;
	
		case TELNET_EV_ERROR:
			endwin();
			fprintf(stderr, "TELNET error: %s\n", ev->error.msg);
			exit(1);
	
		default:
			break;
	}
}
	
/* send a line to the server */
static void send_line (const char* line, size_t len) 
{
	telnet_printf(telnet, "%.*s\n", (int)len, line);
}

/* send NAWS update */
static void send_naws (void) 
{
	unsigned short w = htons(COLS), h = htons(LINES);

	/* send NAWS if enabled */
	if (terminal.flags & TERM_FLAG_NAWS) 
	{
		telnet_begin_sb(telnet, TELOPT_NAWS);
		telnet_send(telnet, (char*)&w, 2);
		telnet_send(telnet, (char*)&h, 2);
		telnet_finish_sb(telnet);
	}
}


