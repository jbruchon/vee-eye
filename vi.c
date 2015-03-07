/*
 * vee-eye: Jody Bruchon's clone of 'vi'
 * Copyright (C) 2015 by Jody Bruchon <jody@jodybruchon.com>
 * Distributed under the MIT License (see LICENSE for details)
 *
 * This clone of 'vi' works with a doubly linked list of text lines.
 * While it may be a bad idea in certain rare circumstances, the use
 * of individual lines as the basic unit of data makes a lot of work
 * in the code easier. The cursor is the "focal point" in this vi;
 * everything is calculated relative to the cursor and the window.
 * Lines longer than the terminal width are handled using a global
 * "line shift" that pushes the screen to the right.
 *
 * I created it mainly as a challenge to learn and improve my skills
 * in C. It is not a very serious project, but if it is useful to
 * you, please let me know! I'll be happy to hear about it. :-)
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/* FIXME: This is only for testing purposes. */
static const char initial_line[] = "Now is the time for all good men to come to the aid of the party. Lorem Ipsum is a stupid Microsoft thing. BLAH BLAH BLAH!";


/* Function prototypes */
static void read_term_dimensions(void);
static void clean_abort(void);
static void update_status(void);
static void redraw_screen(void);


/* Text is stored line-by-line. Line lengths are stored with the text data
 * to avoid lots of strlen() calls for line wrapping, insertion, etc. */
struct line {
	struct line *prev;
	struct line *next;
	char *text;
	int len;
	int alloc_size;
};
static struct line *line_head = NULL;

/* Terminal configuration data */
static struct termios term_orig, term_config;
static int termdesc = -1;
static int term_rows;
static int term_real_rows;
static int term_cols;


/* Current editing locations (screen and file) */
static int crsr_x, crsr_y, cur_line, line_shift;
static char crsr_set_string[12];
/* Track current line's struct pointer to avoid extra walks */
static struct line *cur_line_s = NULL;

/* Maximum size of command mode commands */
#define MAX_CMDSIZE 128

/* Current mode: 0=command, 1=insert, 2=replace */
#define MODE_COMMAND 0
#define MODE_INSERT 1
#define MODE_REPLACE 2
static int vi_mode = 0;
static const char * const mode_string[] = {
	"", "--- INSERT ---", "--- REPLACE ---"
};
#define MAX_STATUS 64
static char custom_status[MAX_STATUS] = "";

/* Total number of lines allocated */
static int line_count = 0;

/* Escape sequence function definitions */
#define CLEAR_SCREEN() write(STDOUT_FILENO, "\033[H\033[J", 6);
#define ERASE_LINE() write(STDOUT_FILENO, "\033[2K", 4);
#define ERASE_TO_EOL() write(STDOUT_FILENO, "\033[K", 3);
#define CRSR_HOME() write(STDOUT_FILENO, "\033[H", 3);
#define CRSR_UP() write(STDOUT_FILENO, "\033[1A", 4);
#define CRSR_DOWN() write(STDOUT_FILENO, "\033[1B", 4);
#define CRSR_LEFT() write(STDOUT_FILENO, "\033[1D", 4);
#define CRSR_RIGHT() write(STDOUT_FILENO, "\033[1C", 4);
#define CRSR_RESTORE() { sprintf(crsr_set_string, "\033[%d;%df", crsr_y, crsr_x); \
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string)); }
#define CRSR_YX(a,b) { sprintf(crsr_set_string, "\033[%d;%df", a, b); \
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string)); }
#define DISABLE_LINE_WRAP() write(STDOUT_FILENO, "\033[7l", 4);
#define ENABLE_LINE_WRAP() write(STDOUT_FILENO, "\033[7h", 4);

/***************************************************************************/

#ifndef NO_SIGNALS
/* Window size change handler */
void sigwinch_handler(int signum, siginfo_t *sig, void *context)
{
	fprintf(stderr, "Got a WINCH\n");
	read_term_dimensions();
	redraw_screen();
	return;
}
#endif	/* NO_SIGNALS */


/* Read terminal dimensions */
static void read_term_dimensions(void)
{
	struct winsize w;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	term_real_rows = w.ws_row;
	term_rows = w.ws_row - 1;
	term_cols = w.ws_col;

	/* Prevent dimensions from being too small */
	if (term_real_rows < 1) term_real_rows = 1;
	if (term_rows < 1) term_rows = 1;
	if (term_cols < 1) term_cols = 1;

	return;
}


/* Invalid command warning */
static void invalid_command(void)
{
	strcpy(custom_status, "Invalid command");
	update_status();
	return;
}


/* Reduce line shift and redraw entire screen */
static void line_shift_reduce(int count)
{
	if (count >= line_shift) line_shift = 0;
	else line_shift -= count;
	redraw_screen();
	return;
}


/* Increase line shift and redraw entire screen */
static void line_shift_increase(int count)
{
	line_shift += count;
	redraw_screen();
	return;
}


/* Walk the line list to the requested line */
static inline struct line *walk_to_line(int num)
{
	struct line *line = line_head;
	int i = 1;

	if (num == 0) return NULL;
	while (line != NULL) {
		if (i == num) break;
		line = line->next;
		i++;
	}
	return line;
}


/* Allocate a new line after the selected line */
static struct line *alloc_new_line(int start,
		const char * const restrict new_text)
{
	struct line *prev_line, *new_line;

	/* Cannot open lines out of current range */
	if (start > line_count) return NULL;

	if (start > 0) prev_line = walk_to_line(start);
	else prev_line = line_head;

	/* Insert a new line */
	new_line = (struct line *)malloc(sizeof(struct line));
	if (!new_line) goto oom;
	if (prev_line == NULL) {
		/* If line_head is NULL, no lines exist yet */
		line_head = new_line;
		new_line->next = NULL;
		new_line->prev = NULL;
	} else {
		/* Insert this line after the existing one */
		new_line->next = prev_line->next;
		new_line->prev = prev_line;
		prev_line->next = new_line;
	}

	/* If inserting between two lines, link the next one to us */
	if (new_line->next != NULL) new_line->next->prev = new_line;

	/* Allocate the text area (if applicable) */
	if (new_text == NULL) {
		new_line->len = 0;
		new_line->text = (char *)malloc(term_cols + 1);
		if (!new_line->text) goto oom;
		*(new_line->text) = '\0';
		new_line->alloc_size = term_cols + 1;
	} else {
		new_line->len = strlen(new_text);
		if (new_line->len < term_cols + 1) {
			new_line->alloc_size = term_cols + 1;
			new_line->text = (char *)malloc(term_cols + 1);
		} else {
			new_line->alloc_size = new_line->len + 1;
			new_line->text = (char *)malloc(new_line->len + 1);
		}
		if (!new_line->text) goto oom;
		strncpy(new_line->text, new_text, new_line->len);
	}

	line_count++;

	return new_line;
oom:
	fprintf(stderr, "out of memory\n");
	clean_abort();

	return NULL;
}

/* Destroy and free one line in the line list */
static int destroy_line(int num)
{
	struct line *line;

	line = walk_to_line(num);
	if (line == NULL) return -1;
	if (line->text != NULL) free(line->text);
	line->prev->next = line->next;
	free(line);

	return 0;
}

static void destroy_buffer(void)
{
	struct line *line = line_head;
	struct line *prev = NULL;

	line_head = NULL;

	/* Free lines in order until list is exhausted */
	while (line != NULL) {
		if (line->text != NULL) free(line->text);
		if (line->prev != NULL) free(line->prev);
		prev = line;
		line = line->next;
	}
	/* Free the final line, if applicable */
	if (prev != NULL) free(prev);
}

static void update_status(void)
{
	char num[4];
	int top_line;

	/* Move the cursor to the last line */
	CRSR_YX(term_real_rows, 0);
	ERASE_LINE();

	/* Print the current insert/replace mode or special status */
	if (*custom_status == '\0')
		strcpy(custom_status, mode_string[vi_mode]);
	write(STDOUT_FILENO, custom_status, strlen(custom_status));
	*custom_status = '\0';

	/* Print our location in the current line and file */
	CRSR_YX(term_real_rows, term_cols - 20);
	printf("%d,%d", cur_line, crsr_x + line_shift);
	CRSR_YX(term_real_rows, term_cols - 5);
	top_line = 1 + (cur_line - crsr_y);
	if (top_line < 1) goto error_top_line;
	if (top_line == 1) {
		write(STDOUT_FILENO, " Top", 4);
	} else if ((cur_line + term_rows) >= line_count) {
		write(STDOUT_FILENO, " Bot", 4);
	} else {
		sprintf(num, "%d%%", (line_count * 100) / top_line);
		write(STDOUT_FILENO, num, strlen(num));
	}

	/* Put the cursor back where it was before we touched it */
	CRSR_RESTORE();

	return;

error_top_line:
	fprintf(stderr, "error: top line is invalid (%d, %d, %d)\n", top_line, cur_line, crsr_y);
	clean_abort();
}


/* Write a line to the screen with appropriate shift */
static void write_shifted_line(struct line *line, int y)
{
	char *p = line->text + line_shift;
	int len = line->len - line_shift;

	sprintf(crsr_set_string, "\033[%d;1f", y);
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string));
	if (len > term_cols) len = term_cols;
	write(STDOUT_FILENO, p, len);
	if (crsr_y != term_cols) ERASE_TO_EOL();
	write(STDOUT_FILENO, "\n", 1);
	return;
}


/* Redraw the entire screen */
static void redraw_screen(void)
{
	struct line *line;
	int start_y;
	int remain_row;

	CLEAR_SCREEN();

	/* Get start line number and pointer */
	if (cur_line < crsr_y) goto error_line_cursor;
	start_y = cur_line + 1 - crsr_y;

	/* Find the first line to write to the screen */
	line = walk_to_line(start_y);
	if (!line) goto error_line_walk;

	/* Draw lines until no more are left */
	remain_row = term_rows;
	while (remain_row) {
		/* Write out this line data */
		write_shifted_line(line, (term_rows - remain_row + 1));
//		CRSR_DOWN();
		remain_row--;
		line = line->next;
		if (line == NULL) break;
	}

	/* Fill the rest of the screen with tildes */
	while (remain_row) {
		write(STDOUT_FILENO, "~\n", 2);
		remain_row--;
	}

	update_status();
	CRSR_HOME();

	return;

error_line_cursor:
	fprintf(stderr, "error: cur_line < crsr_y\n");
	clean_abort();
error_line_walk:
	fprintf(stderr, "error: line walk invalid (%d) (%d - %d)\n",
			start_y, cur_line, crsr_y);
}


/* Delete char at cursor location */
static void do_del_under_crsr(void)
{
	char *p;

	if (cur_line_s->len == 0) return;
	p = cur_line_s->text + crsr_x + line_shift;

	/* Copy everything down one char */
	strncpy(p - 1, p, strlen(p));
	cur_line_s->len--;
	if (crsr_x > (cur_line_s->len - line_shift)) crsr_x--;
	if (crsr_x < 1) {
		if (line_shift > 0) line_shift_reduce(1);
		crsr_x = 1;
	}
	write_shifted_line(cur_line_s, crsr_y);
	CRSR_RESTORE();
	return;
}


/* Restore terminal to original configuration */
static void term_restore(void)
{
	if (termdesc != -1) tcsetattr(termdesc, TCSANOW, &term_orig);
	ENABLE_LINE_WRAP();
	return;
}

/* Initialize terminal settings */
static int term_init(void)
{
	/* Only init terminal once */
	if (termdesc != -1) return 0;

	/* Find a std* stream with a TTY */
	if (isatty(STDIN_FILENO)) termdesc = STDIN_FILENO;
	else if (isatty(STDOUT_FILENO)) termdesc = STDOUT_FILENO;
	else if (isatty(STDERR_FILENO)) termdesc = STDERR_FILENO;
	else return -ENOTTY;

	/* Get current terminal configuration and save it*/
	if (tcgetattr(termdesc, &term_orig)) return -EBADF;
	memcpy(&term_config, &term_orig, sizeof(struct termios));

	/* Disable buffering */
	if (isatty(STDIN_FILENO)) setvbuf(stdin, NULL, _IONBF, 0);
	if (isatty(STDOUT_FILENO)) setvbuf(stdout, NULL, _IONBF, 0);
	if (isatty(STDERR_FILENO)) setvbuf(stderr, NULL, _IONBF, 0);

	/* Configure terminal settings */

	/* c_cc */
	term_config.c_cc[VTIME] = 0;
	term_config.c_cc[VMIN] = 1;	/* read() one char at a time*/

	/* iflag */
#ifdef IUCLC
	term_config.c_iflag &= ~IUCLC;
#endif
	term_config.c_iflag &= ~PARMRK;
	term_config.c_iflag |= IGNPAR;
	term_config.c_iflag &= ~IGNBRK;
	term_config.c_iflag |= BRKINT;
	term_config.c_iflag &= ~ISTRIP;
	term_config.c_iflag &= ~(INLCR | IGNCR | ICRNL);

	/* cflag */
	term_config.c_cflag &= ~CSIZE;
	term_config.c_cflag |= CS8;
	term_config.c_cflag |= CREAD;

	/* lflag */
	term_config.c_lflag |= ISIG;
	term_config.c_lflag &= ~ICANON;	/* disable line buffering */
	term_config.c_lflag &= ~IEXTEN;

	/* disable local echo */
	term_config.c_lflag &= ~(ECHO | ECHONL | ECHOE | ECHOK);

	/* Finalize settings */
	tcsetattr(termdesc, TCSANOW, &term_config);

	/* Disable automatic line wrapping */
	DISABLE_LINE_WRAP();

	return 0;
}

/* Clean abort */
static void clean_abort(void)
{
	term_restore();
	destroy_buffer();
	exit(EXIT_FAILURE);
}


/* Oh dear God, NO! */
static void oh_dear_god_no(char *string)
{
	strcpy(custom_status, "THIS SHOULDN'T HAPPEN: ");
	strcat(custom_status, string);
	update_status();
	return;
}


void insert_char(char c)
{
	char *new_text;
	char *p;

	switch (vi_mode) {
	case 1:	/* insert mode */
		if (cur_line_s->alloc_size == (cur_line_s->len)) {
			/* Allocate a double size buffer and insert to that*/
			new_text = (char *)realloc(cur_line_s->text, cur_line_s->len << 1);
			if (!new_text) goto oom;
			cur_line_s->text = new_text;
			cur_line_s->alloc_size = cur_line_s->len << 1;
		}
		/* Move text up by one byte */
		p = cur_line_s->text + crsr_x + line_shift - 1;
		memmove(p + 1, p, strlen(p) + 1);
		*p = c;
		crsr_x++;
		cur_line_s->len++;
		return;

	case 2: /* replace mode */
		return;
	}

	return;
oom:
	fprintf(stderr, "error: out of memory\n");
	clean_abort();
	return;
}


/* Editing mode. Doesn't return until ESC pressed. */
void edit_mode(void)
{
	char c;
	while (read(STDIN_FILENO, &c, 1)) {
		switch (c) {
		case '\0':
			continue;

		case '\b':
		case 0x7f:
			if (crsr_x > 1) {
				crsr_x--;
				/* FIXME: Add joining of lines on backspace */
				do_del_under_crsr();
			}
			continue;

		case '\n':
		case '\r':
			cur_line_s = alloc_new_line(cur_line, NULL);
			cur_line++;
			crsr_y++;
			crsr_x = 1;
			continue;

		case '\033':
			/* FIXME: poll for ESC sequences */
			crsr_x--;
			vi_mode = MODE_COMMAND;
			update_status();
			return;

		default:
			break;
		}

		/* Catch any invalid characters */
		if (c < 32 || c > 127) {
			sprintf(custom_status, "Invalid char entered: %u", c);
			update_status();
			continue;
		}
		/* Insert character at cursor position */
		insert_char(c);
		write_shifted_line(cur_line_s, crsr_y);
		CRSR_RESTORE();
	}
	return;
}


/* Get a free-form command string from the user */
static int get_command_string(char *command)
{
	int cmdsize = 0;
	char cc;

	while (read(STDIN_FILENO, &cc, 1)) {
		/* If user presses ESC, abort */
		if (cc == '\033') {
			command[0] = '\0';
			return 0;
		}

		/* Backspace */
		if (cc == '\b' || cc == 0x7f) {
			command[cmdsize] = '\0';
			cmdsize--;
			if (cmdsize < 0) return 0;;
			write(STDOUT_FILENO, "\b \b", 3);
			continue;
		}

		/* Newline or carriage return */
		if (cc == '\n' || cc == '\r') {
			command[cmdsize] = '\0';
			break;
		}
		write(STDOUT_FILENO, &cc, 1);
		command[cmdsize] = cc;
		cmdsize++;
		if (cmdsize == MAX_CMDSIZE) break;
	}
	return cmdsize;
}


static void do_cursor_left(void)
{
	if (crsr_x == 1) {
		if (line_shift > 0) {
			line_shift_reduce(1);
			write_shifted_line(cur_line_s, crsr_y);
		}
		return;
	}
	crsr_x--; CRSR_LEFT();
	return;
}

static void do_cursor_right(void)
{
	if (crsr_x == cur_line_s->len) return;
	if (crsr_x == term_cols) {
		if ((cur_line_s->len - line_shift) > term_cols) {
			line_shift_increase(1);
			write_shifted_line(cur_line_s, crsr_y);
		} else {
			oh_dear_god_no("cmd: l: term_cols check");
			return;
		}
	} else {
		if (crsr_x < (cur_line_s->len - line_shift)) {
			crsr_x++;
			CRSR_RIGHT();
		}
	}
	return;
}


static void do_cursor_up(void)
{
	if (cur_line == 1) return;
	if (cur_line_s->prev == NULL) return;
	cur_line_s = cur_line_s->prev;
	crsr_y--;
	cur_line--;
	if ((crsr_x + line_shift) > cur_line_s->len) {
		if (cur_line_s->len <= line_shift)
			line_shift = cur_line_s->len - 1;
		crsr_x = cur_line_s->len - line_shift;
		if (crsr_x == 0) crsr_x = 1;
		redraw_screen();
	}

	return;
}


static void do_cursor_down(void)
{
	if (cur_line == line_count) return;
	if (cur_line_s->next == NULL) return;
	cur_line_s = cur_line_s->next;
	crsr_y++;
	cur_line++;
	if ((crsr_x + line_shift) > cur_line_s->len) {
		if (cur_line_s->len <= line_shift)
			line_shift = cur_line_s->len - 1;
		crsr_x = cur_line_s->len - line_shift;
		if (crsr_x == 0) crsr_x = 1;
		redraw_screen();
	}

	return;
}


/* Handle an incoming command */
int do_cmd(char c)
{
	char command[MAX_CMDSIZE];

	switch (c) {
	case 'q':
		goto end_vi;
		break;
	case 'a':
		crsr_x++;
	case 'i':
		vi_mode = MODE_INSERT;
		update_status();
		edit_mode();
		break;
	case 'h':	/* left */
		do_cursor_left();
		break;
	case 'j':	/* down */
		do_cursor_down();
		break;
	case 'k':	/* up */
		do_cursor_up();
		break;
	case 'l':	/* right */
		do_cursor_right();
		break;
	case 'x':	/* Delete char at cursor */
		do_del_under_crsr();
		break;
	case '!':	/* NON-STANDARD cursor pos dump */
		sprintf(custom_status, "%dx%d, cx %d, cy %d, lin %d, cur %d, cll %d, cas %d",
				term_cols, term_real_rows, crsr_x, crsr_y, line_count, cur_line,
				cur_line_s->len, cur_line_s->alloc_size);
		break;
	case ':':	/* Colon command */
		CRSR_YX(term_real_rows, 1);
		ERASE_LINE();
		write(STDOUT_FILENO, ":", 1);
		if (!get_command_string(command)) break;
		if (strcmp(command, "q") == 0) goto end_vi;
		break;
	default:
		sprintf(custom_status, "Unknown key %u", c);
		break;
	}
	CRSR_RESTORE();
	update_status();
	return 0;

end_vi:
	CRSR_YX(term_real_rows, 1);
	ERASE_LINE();
	term_restore();
	destroy_buffer();
	exit(EXIT_SUCCESS);
}


int main(int argc, char **argv)
{
	int i;
	char c;
	struct sigaction act;

#ifndef NO_SIGNALS
	/* Set up SIGWINCH handler for window resizing support */
	memset(&act, 0, sizeof(struct sigaction));
	sigemptyset(&act.sa_mask);
	act.sa_sigaction = sigwinch_handler;
	act.sa_flags = SA_SIGINFO;
	sigaction(SIGWINCH, &act, NULL);
#endif	/* NO_SIGNALS */

	line_shift = 0;
	if ((i = term_init()) != 0) {
		if (i == -ENOTTY) fprintf(stderr, "tty is required\n");
		else fprintf(stderr, "cannot init terminal: %s\n", strerror(-i));
		clean_abort();
	}

	read_term_dimensions();
	CLEAR_SCREEN();

	/* Set up the testing line(s) */
	cur_line_s = alloc_new_line(line_count, initial_line);
	if (!cur_line_s) {
		fprintf(stderr, "Cannot create initial line\n");
		clean_abort();
	}
	if (!alloc_new_line(line_count, "test")) {
		fprintf(stderr, "Cannot create second line\n");
		clean_abort();
	}

	crsr_x = 1; crsr_y = 1;
	cur_line = 1;
	redraw_screen();
	/* Read commands forever */
	while (read(STDIN_FILENO, &c, 1)) do_cmd(c);
	clean_abort();
}
