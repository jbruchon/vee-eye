/*
 * vee-eye: Jody Bruchon's clone of 'vi'
 * Copyright (C) 2015-2020 by Jody Bruchon <jody@jodybruchon.com>
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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#ifndef NO_SIGNALS
 #include <signal.h>
#endif	/* NO_SIGNALS */

/* Dev86 used for ELKS isn't C99 compliant */
#ifdef __ELKS__
 #define uintptr_t unsigned short
 #define restrict
 #define inline
 #define PATH_MAX 256
#else
 #include <stdint.h>
#endif	/* __ELKS__ */


/* The current movement command parameters (cursor-relative)
 * Movement is stored as either a destination line/char or a count of
 * chars relative to the cursor. When all three numbers are zero, there
 * is no movement stored. We store movements in this way so that any
 * command which consists of an operation and a movement can use any
 * valid movement to modify its operations. dest_line set with
 * dest_char==0 is a special case: if no dest_char is specified, the
 * movement operates on full lines. An example: if the user types the
 * 'dd' command, the movement specification will be dest_line = 1,
 * dest_char = 0. This will perform the 'd' command (delete) on all lines
 * starting at cur_line until it reaches (cur_line + dest_line). Likewise,
 * if a 'dw' is performed, the word movement portion will scan for the
 * next word break and specify its exact location relative to the cursor;
 * for example, if the cursor is on the third letter of "birds" in the
 * two line area:
 *
 * ...blah blah blah, but when birds
 * attack me, I run away
 *
 * the movement from the 'r' in 'birds' to the start of the next word
 * will break across a line and the movement will be stored as:
 * dest_line = 1, dest_char = 1. Basically, dest_char works as a flag
 * to indicate if the ranging is line-by-line or char-by-char.
 *
 * start_line and start_char are almost always set to be equal to cur_line
 * and (crsr_x + line_shift) respectively (these are the cursor's current
 * (X,Y) coordinates in the whole file.)
 */
static struct {
	int start_line;
	int start_char;
	int dest_line;
	int dest_char;
} cur_movement;

/* Text is stored line-by-line. Line lengths are stored with the text data
 * to avoid lots of strlen() calls for line wrapping, insertion, etc.
 * alloc_size is the total allocated size of the text[] element. This
 * program always allocates more space than is required for each line so
 * that subsequent edit operations minimize allocations. */
struct line {
	struct line *prev;
	struct line *next;
	char *text;
	int len;
	int alloc_size;
};
static struct line *line_head = NULL;

/* Yank buffer */
static struct line *yank_head = NULL;
static int yank_line_count = 0;

/* Terminal configuration data */
static struct termios term_orig, term_config;
static int termdesc = -1;
static int term_rows;
static int term_real_rows;
static int term_cols;


/* Current editing locations (screen and file) */
static int crsr_x, crsr_y, cur_line, line_shift;
#define MAX_CRSR_SETSTRING 16
static char crsr_set_string[MAX_CRSR_SETSTRING];

/* Current file name */
static char curfile[PATH_MAX];

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
	"", "-- INSERT --", "-- REPLACE --"
};
#define MAX_STATUS 64
static char custom_status[MAX_STATUS] = "";

/* Total number of lines allocated */
static int line_count = 0;

/* File read/write buffer */
#define CHUNK_SIZE 4096
static char buf[CHUNK_SIZE];

/* Escape sequence function definitions */
#define CLEAR_SCREEN()	write(STDOUT_FILENO, "\033[H\033[J", 6);
#define ERASE_LINE()	write(STDOUT_FILENO, "\033[2K", 4);
#define ERASE_TO_EOL()	write(STDOUT_FILENO, "\033[K", 3);
#define CRSR_HOME()	write(STDOUT_FILENO, "\033[H", 3);
#define CRSR_UP()	write(STDOUT_FILENO, "\033[1A", 4);
#define CRSR_DOWN()	write(STDOUT_FILENO, "\033[1B", 4);
#define CRSR_LEFT()	write(STDOUT_FILENO, "\033[1D", 4);
#define CRSR_RIGHT()	write(STDOUT_FILENO, "\033[1C", 4);
#define SCROLL_UP()	crsr_yx(1,1); write(STDOUT_FILENO, "\033M", 2); crsr_restore();
#define SCROLL_DOWN()	crsr_yx(term_real_rows,1); write(STDOUT_FILENO, "\033D", 2); crsr_restore();
#define DISABLE_LINE_WRAP()	write(STDOUT_FILENO, "\033[7l", 4);
#define ENABLE_LINE_WRAP() 	write(STDOUT_FILENO, "\033[7h", 4);


/* Function prototypes */
static void read_term_dimensions(void);
static void clean_abort(void);
static void update_status(void);
static void redraw_screen(int row_start, int row_end);
static void destroy_buffer(struct line **head);
static void do_cursor_up(void);
static void do_cursor_down(void);
static void do_cursor_left(void);
static void do_cursor_right(void);


/***************************************************************************/


/* Oh dear God, NO! */
static void oh_dear_god_no(char *string)
{
	strncpy(custom_status, "THIS SHOULDN'T HAPPEN: ", MAX_STATUS);
	strncat(custom_status, string, MAX_STATUS);
	update_status();
	return;
}


/* Cursor control functions */
void crsr_restore(void)
{
	sprintf(crsr_set_string, "\033[%d;%df", crsr_y, crsr_x);
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string));
}

void crsr_yx(int row, int col)
{
	sprintf(crsr_set_string, "\033[%d;%df", row, col);
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string));
}

static inline void set_scroll_area(void) {
	sprintf(crsr_set_string, "\033[%d;%dr", 1, term_rows);
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string));
}

#ifndef NO_SIGNALS
/* Window size change handler */
void sigwinch_handler(int signum, siginfo_t *sig, void *context)
{
	//fprintf(stderr, "Got a WINCH\n");
	sleep(1);
	read_term_dimensions();
	set_scroll_area();
	if (crsr_x >= term_cols) crsr_x = term_cols - 1;
	if (crsr_y >= term_rows) crsr_y = term_rows - 1;
	crsr_restore();
	redraw_screen(0, 0);
	sprintf(custom_status, "Terminal resized to %dx%d", term_cols, term_rows);
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


/* Write a line to the screen with appropriate shift */
static void redraw_line(struct line *line, int y)
{
	char *p;
	int len;

	if (!line) goto error_line_null;
	if (!line->text) goto error_text_null;
	p = line->text + line_shift;
	len = line->len - line_shift;
	sprintf(crsr_set_string, "\033[%d;1f", y);
	//ERASE_TO_EOL();
	write(STDOUT_FILENO, crsr_set_string, strlen(crsr_set_string));
	if (len > term_cols) len = term_cols;
	if (len > 0) write(STDOUT_FILENO, p, len);
	crsr_yx(y, len + 1);
	if (len < term_cols) ERASE_TO_EOL();
	crsr_restore();
	//write(STDOUT_FILENO, "\n", 1);
	//sleep(1);
	return;

error_line_null:
	fprintf(stderr, "error: redraw_line line is NULL\n");
	clean_abort();
error_text_null:
	fprintf(stderr, "error: redraw_line line->text is NULL\n");
	clean_abort();
}


/* Reduce line shift and redraw entire screen */
static inline void line_shift_reduce(int count)
{
	if (count == 0 || count >= line_shift) line_shift = 0;
	else line_shift -= count;
	//redraw_screen(0, 0);
	redraw_line(cur_line_s, crsr_y);
	return;
}


/* Increase line shift and redraw entire screen */
static inline void line_shift_increase(int count)
{
	line_shift += count;
	//redraw_screen(0, 0);
	redraw_line(cur_line_s, crsr_y);
	return;
}


/* Walk the specified line list to the requested line */
static inline struct line *walk_to_line(int num,
		struct line *line)
{
	int i = 1;

	if (num == 0 || line == NULL) return NULL;

	while (line != NULL) {
		if (i == num) break;
		if (line->next == NULL) return NULL;
		line = line->next;
		i++;
	}
	return line;
}


/* Out of memory */
void oom(void) {
	strcpy(custom_status, "out of memory");
	update_status();
	clean_abort();
}


/* Allocate a new line after the selected line */
static struct line *alloc_new_line(int start,
		const char * const restrict new_text,
		int *buf_line_count,
		struct line **buf_head)
{
	struct line *prev_line, *new_line;
	int new_ll;

	/* Cannot open lines out of current range */
	if (start > *buf_line_count) return NULL;

	if (start > 0) prev_line = walk_to_line(start, *buf_head);
	else prev_line = *buf_head;

	/* Insert a new line */
	new_line = (struct line *)malloc(sizeof(struct line));
	if (!new_line) oom();
	if (prev_line == NULL) {
		/* If buf_head is NULL, no lines exist yet */
		*buf_head = new_line;
		new_line->next = NULL;
		new_line->prev = NULL;
	} else {
		/* Insert this line after the existing one */
		new_line->next = prev_line->next;
		new_line->prev = prev_line;
		prev_line->next = new_line;
		if (new_line->next != NULL)
			new_line->next->prev = new_line;
	}

	/* If inserting between two lines, link the next one to us */
	if (new_line->next != NULL) new_line->next->prev = new_line;

	/* Allocate the text area (if applicable) */
	if (new_text == NULL) {
		new_line->len = 0;
		new_line->text = (char *)calloc(1, 32);
		if (!new_line->text) oom();
		new_line->alloc_size = 32;
	} else {
		new_line->len = strlen(new_text);
		/* Allocate in 32-byte chunks: add null terminator room then
		 * shift-divide; increment and shift-multiply to get the
		 * number of bytes we need to allocate. */
		new_ll = ((new_line->len + 1) >> 5) + 1;
		new_ll <<= 5;
		new_line->alloc_size = new_ll;
		new_line->text = (char *)calloc(1, new_ll);
		if (!new_line->text) oom();
		strcpy(new_line->text, new_text);
	}

	*buf_line_count += 1;

	return new_line;
}


/* From the current line, yank data into the yank buffer
 * If this_text is set, a single line (string) will be yanked from
 * this_text and no complex movement work will be done.
 * If lines are specified, chars are ignored. If this is called with
 * yank_add == 0, the yank buffer is destroyed and replaced. */
static int yank(char *this_text)
{
	struct line *yank_line;
	int yank_cur_line = 1;

return 0; /* FIXME: finish this code */

	/* Destroy the yank buffer and allocate a new one */
	destroy_buffer(&yank_head);
	yank_line_count = 0;

	if (this_text != NULL) {
		/* Special case: yank the line pointed to directly */
		yank_head = alloc_new_line(0, this_text, &yank_line_count, &yank_head);
		return 0;
	} else {
		/* Yanking chars or more than just a single explicit line */
		yank_head = alloc_new_line(0, NULL, &yank_line_count, &yank_head);
	}
	/* Yank the range indicated by cur_movement */
	return 0;
}


/* Destroy and free one line in the line list */
static int destroy_line(struct line *target_line)
{
	struct line *temp_line;

	if (target_line == NULL) return -1;
	if (target_line->prev != NULL) {
		if (target_line->text != NULL) free(target_line->text);
		/* Detach the line to be destroyed from the list */
		target_line->prev->next = target_line->next;
		if (target_line->next != NULL)
			target_line->next->prev = target_line->prev;
		/* Jump to the next line and destroy the previous one */
		free(target_line);
		line_count--;
	} else {
		/* Line 1 must be handled differently */
		if (line_count > 1) {
			if (target_line->next == NULL) goto error_line_null;
			temp_line = target_line;
			target_line = target_line->next;
			target_line->prev = NULL;
			if (temp_line->text != NULL) free(temp_line->text);
			free(temp_line);
			/* Update line_head if we just destroyed it */
			if ((uintptr_t)temp_line == (uintptr_t)line_head)
				line_head = target_line;
			line_count--;
		} else {
			/* Line 1 with no more lines */
			target_line->len = 0;
			/* Warn if user tried to delete the only empty line */
			if (*target_line->text == '\0') return 1;
			*(target_line->text) = '\0';
		}
	}

	redraw_screen(0, 0);

	return 0;

error_line_null:
	fprintf(stderr, "error: line 1 next ptr = NULL but line_count > 1\n");
	clean_abort();
	return -1;
}


/* Destroy every line in the selected buffer
 * This is used to empty the yank buffer and to de-allocate the line buffer
 * on program exit; use it like this: destroy_buffer(&buffer_head); */
static void destroy_buffer(struct line **head)
{
	struct line *line = *head;
	struct line *prev = NULL;

	*head = NULL;

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
	crsr_yx(term_real_rows, 0);
	ERASE_LINE();

	/* Print the current insert/replace mode or special status */
	if (*custom_status == '\0')
		strncpy(custom_status, mode_string[vi_mode], MAX_STATUS);
	write(STDOUT_FILENO, custom_status, strlen(custom_status));
	*custom_status = '\0';

	/* Print our location in the current line and file */
	crsr_yx(term_real_rows, term_cols - 16);
	printf("%d,%d", cur_line, crsr_x + line_shift);
	crsr_yx(term_real_rows, term_cols - 5);
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
	crsr_restore();

	return;

error_top_line:
	fprintf(stderr, "\n\nerror: top line is invalid (%d, %d, %d)\n",
			top_line, cur_line, crsr_y);
	clean_abort();
}


/* Redraw part or all of the screen */
static void redraw_screen(int row_start, int row_end)
{
	struct line *line;
	int start_y;
	int this_row;

	if (cur_line < crsr_y) goto error_line_cursor;
	if (row_start > term_rows) goto error_row_params;

	if (row_start <= 0) row_start = 1;
	if (row_end <= 0) row_end = term_rows;

	if (row_start == 1 && row_end == term_rows) CLEAR_SCREEN();

	/* Get start line number and pointer */
	start_y = cur_line - crsr_y + row_start;

	/* Find the first line to write to the screen */
	line = walk_to_line(start_y, line_head);
	if (!line) goto error_line_walk;
//	fprintf(stderr, "line walk: start_y %d, line_head %p, line %p, line->next %p, re %u, rs %u\n", start_y, line_head, line, line->next, row_end, row_start);
//	clean_abort();

	/* Draw lines until no more are left */
	this_row = row_start;
	while (this_row <= row_end) {
		/* Write out this line data */
//		fprintf(stderr, "\nstart_y %d, redraw_line line %p, this_row %u, re %u, rs %u", start_y, line, this_row, row_end, row_start);
		redraw_line(line, this_row);
		this_row++;
		crsr_yx(this_row, 1);
		line = line->next;
		if (line == NULL) break;
	}

	/* Fill the rest of the screen with tildes */
	while (this_row <= row_end) {
		write(STDOUT_FILENO, "~\n", 2);
		this_row++;
	}

	//update_status();
	//CRSR_HOME();
	crsr_restore();

	return;

error_line_cursor:
	fprintf(stderr, "error: cur_line < crsr_y\n");
	clean_abort();
error_row_params:
	fprintf(stderr, "error: bad row params: %u, %u\n",
			row_start, row_end);
	clean_abort();
error_line_walk:
	fprintf(stderr, "error: line walk invalid (%d) (%d - %d)\n",
			start_y, cur_line, crsr_y);
	clean_abort();
}


/* Delete char at cursor location */
static int do_del_under_crsr(int left)
{
	char *p;

	if (cur_line_s->len == 0) return 1;
	if (crsr_x > (cur_line_s->len + line_shift) && left == 0) return 1;
	p = cur_line_s->text + crsr_x + line_shift;

	/* Copy everything down one char */
	memmove(p - 1, p, strlen(p) + 1);
	cur_line_s->len--;
	if (crsr_x > (cur_line_s->len - line_shift) + (vi_mode > 0 ? 1 : 0)) crsr_x--;
	if (crsr_x < 1) {
		if (line_shift > 0) line_shift_reduce(1);
		crsr_x = 1;
	}
	redraw_line(cur_line_s, crsr_y);
	crsr_restore();
	return 0;
}


static void go_to_start_of_next_line(void)
{
	do_cursor_down();
	line_shift = 0;
	crsr_x = 1;
	crsr_restore();
	update_status();
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

	set_scroll_area();

	return 0;
}

/* Clean abort */
static void clean_abort(void)
{
	term_restore();
	destroy_buffer(&line_head);
	destroy_buffer(&yank_head);
	exit(EXIT_FAILURE);
}


void insert_char(char c)
{
	char *new_text;
	char *p;

	switch (vi_mode) {
	case 1:	/* insert mode */
		if (cur_line_s->alloc_size == (cur_line_s->len)) {
			/* Allocate a larger buffer and insert to that */
			new_text = (char *)realloc(cur_line_s->text, cur_line_s->len << 1);
			if (!new_text) oom();
			cur_line_s->text = new_text;
			cur_line_s->alloc_size = cur_line_s->len << 1;
		}
		/* Move text up by one byte */
		p = cur_line_s->text + crsr_x + line_shift - 1;
		memmove(p + 1, p, strlen(p) + 1);
		*p = c;
		if (crsr_x > term_cols) line_shift_increase(1);
		else crsr_x++;
		cur_line_s->len++;
		return;

	case 2: /* replace mode */
		return;
	}

	return;
}


/* Editing mode. Doesn't return until ESC pressed. */
void edit_mode(void)
{
	unsigned char c;
	char *fragment;

	while (read(STDIN_FILENO, &c, 1)) {
		switch (c) {
		case '\0':
			continue;

		case '\b':
		case 0x7f:
			if (crsr_x > 1) {
				crsr_x--;
				/* FIXME: Add joining of lines on backspace */
				do_del_under_crsr(1);
			}
			continue;

		case '\n':
		case '\r':	/* New line */
			fragment = cur_line_s->text + line_shift + crsr_x - 1;
//			sprintf(custom_status, "txt %p, adjtxt %p, ls+cx %d+%d",
//					cur_line_s->text, fragment, line_shift, crsr_x);

			if (!alloc_new_line(cur_line, fragment, &line_count, &line_head)) oom();

			/* New lines need to break the old line apart */
			if (*fragment != '\0') {
				cur_line_s->len = line_shift + crsr_x - 1;
				*fragment = '\0';
			}
			go_to_start_of_next_line();
			redraw_screen(crsr_y, 0);
			continue;

		case '\033':
			/* FIXME: poll for ESC sequences */
			goto end_edit_mode;

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
		redraw_line(cur_line_s, crsr_y);
		crsr_restore();
		update_status();
	}

end_edit_mode:
	if (crsr_x > 1) crsr_x--;
	vi_mode = MODE_COMMAND;
	update_status();
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
			redraw_line(cur_line_s, crsr_y);
		}
		return;
	} else CRSR_LEFT();
	crsr_x--;
	return;
}

static void do_cursor_right(void)
{
	if (crsr_x > cur_line_s->len) return;
	if (crsr_x == term_cols) {
		if ((cur_line_s->len - line_shift) > term_cols) {
			line_shift_increase(1);
			redraw_line(cur_line_s, crsr_y);
		} else {
			oh_dear_god_no("cmd: l: term_cols check");
			return;
		}
	} else {
		/* Only move one space beyond end when in edit mode */
		if ((crsr_x < (cur_line_s->len - line_shift)) ||
			(vi_mode > 0 &&
			crsr_x == (cur_line_s->len - line_shift))) {
			crsr_x++;
			CRSR_RIGHT();
		}
	}
	return;
}


static void do_cursor_up(void)
{
	int temp_shift = line_shift;

	line_shift = 0;
	redraw_line(cur_line_s, crsr_y);
	line_shift = temp_shift;
	if (cur_line == 1) return;
	if (cur_line_s->prev == NULL) return;
	cur_line_s = cur_line_s->prev;
	cur_line--;
	if (crsr_y > 1) crsr_y--;
//	else redraw_screen(0, 0);
	else {
		SCROLL_UP();
	}
	/* Pull the cursor to EOL if it is too far over */
	if (crsr_x > cur_line_s->len) crsr_x = cur_line_s->len;
	line_shift = 0;
	redraw_line(cur_line_s, crsr_y);

	return;
}


static void do_cursor_down(void)
{
	int temp_shift = line_shift;

	line_shift = 0;
	redraw_line(cur_line_s, crsr_y);
	line_shift = temp_shift;
	if (cur_line == line_count) return;
	if (cur_line_s->next == NULL) return;
	cur_line_s = cur_line_s->next;
	cur_line++;
	if (crsr_y < term_rows) crsr_y++;
//	else redraw_screen(0, 0);
	else {
		SCROLL_DOWN();
		redraw_line(cur_line_s, crsr_y);
	}
	/* Pull the cursor to EOL if it is too far over */
	if ((crsr_x + line_shift) > cur_line_s->len) {
		if (cur_line_s->len <= line_shift)
			line_shift = cur_line_s->len - 1;
		crsr_x = cur_line_s->len - line_shift;
		if (crsr_x == 0) crsr_x = 1;
		//redraw_screen(0, 0);
		redraw_line(cur_line_s, crsr_y);
	}

	return;
}


/* Load a file into buffer starting at a particular line */
int load_file(const char * const restrict name, const int start_line)
{
	FILE *fp;
	struct line *cur_load_line;
	int load_line_count = 0;
	int buflen;

	/* start_line = 0 will load at the beginning of the buffer */
	if ((start_line != 0) && (start_line > line_count)) {
//		printf("start_line %d, line_count %d, cur_line %d, name %s\n",
//				start_line, line_count, cur_line, name);
		return -1;
	}
	if (name[0] == '\0') {
		strcpy(custom_status, "No filename specified to load");
		return -2;
	}

	fp = fopen(curfile, "rb");
	if (!fp) return -3;

	cur_load_line = walk_to_line(start_line, line_head);
	if (cur_load_line == NULL) cur_load_line = line_head;
	/* Read the file into the buffer line by line */
	while (feof(fp) == 0) {
		//printf("entry: cll %p, lh %p, sl %d, linecnts %d,%d\n",cur_load_line,line_head,start_line,line_count,load_line_count);
		buf[0] = '\0';
		if (fgets(buf, CHUNK_SIZE, fp) == NULL) {
			fclose(fp);
			break;
		}
		if (ferror(fp) != 0) {
			fclose(fp);
			return -4;
		}

		buflen = strlen(buf) - 1;
		if (buf[buflen] == '\n' || buf[buflen] == '\r') buf[buflen] = '\0';
		cur_load_line = alloc_new_line(0, buf, &line_count, &cur_load_line); // fast way
		if (cur_load_line == NULL) {
			fclose(fp);
			return -5;
		}
		//printf("line %d len %d: %s", line_count, strlen(buf), buf);
		if (line_head == NULL) {
			line_head = cur_load_line;
			line_head->prev = NULL;
		}
		load_line_count++;
		//printf("loop : cll %p, lh %p, sl %d, linecnts %d,%d\n",cur_load_line,line_head,start_line,line_count,load_line_count);
	}

	//exit(0);
	return load_line_count;
}


/* Save the buffer to the file specified */
int save_file(const char * const restrict name)
{
	FILE *fp;
	struct line *line = line_head;

	if (!name || *name == '\0') return -1;
	/* TODO: save the file */
	fp = fopen(name, "w+b");
	if (!fp) return -1;
	while (line) {
		errno = 0;
		fwrite(line->text, line->len, 1, fp);
		if (ferror(fp)) break;
		fwrite("\n", 1, 1, fp);
		if (ferror(fp)) break;
		line = line->next;
	}
	fclose(fp);
	if (errno != 0) return -1;
	return 0;
}


/* Get a movement subcommand
 * Returns 0 on valid movement, -1 on invalid movement or ESC
 * Returns 1 on invalid movement to allow 'dd' 'yy' etc. to work */
int get_movement(char *command, int cmd_len, int num_times)
{
	char c;

	while (read(STDIN_FILENO, &c, 1)) {
		/* Handle numbers first */
		if (c >= '1' && c <= '9') {
		}
		switch(c) {
		case '\033':
		case '\027':
			return -1;
		case 'h':	/* left */
		case 'j':	/* down */
		case 'k':	/* up */
		case 'l':	/* right */
		case 'H':	/* top of screen */
		case 'M':	/* middle of screen */
		case 'L':	/* bottom of screen */
		case 'w':	/* next word */
		case 'W':	/* previous word */
		case 'b':	/* beginning of word */
		case 'B':	/* beginning of blank-delimited word */
		case 'e':	/* end of word */
		case 'E':	/* end of blank-delimited word */
		case '(':	/* one sentence back */
		case ')':	/* one sentence forward */
		case '{':	/* one paragraph back */
		case '}':	/* one paragraph forward */
		case '0':	/* beginning of line */
		case '$':	/* end of line */
		case 'G':	/* last line of file */
		case ':':	/* move to :n line of file */
		case 'f':	/* fc: move forward to char 'c' */
		case 'F':	/* Fc: move backward to char 'c' */
		case '%':	/* move to associated (), {}, or [] */
		default:
			return 1;
		}
	}
	return 0;
}

/* Handle an incoming command */
int do_cmd(char c)
{
	char command[MAX_CMDSIZE];
	char *savefile;
	int cmd_len = 0;
	int num_times = 1;
	int i;

	command[0] = c; cmd_len++;

	//fprintf(stderr, "do_cmd: 0x%x\n", c);
	while (c >= '0' && c <= '9') {
		strncpy(custom_status, command, cmd_len + 1);
		update_status();
		read(STDIN_FILENO, &c, 1);
		command[cmd_len] = c; cmd_len++;
		if (cmd_len == MAX_CMDSIZE - 1) break;
	}

	/* User pressed ESC; cancel command */
	if (c == '\033') goto end_cmd;
	/* ignore other control codes */
	if (c < 32 || c > 127) goto end_cmd;

	command[cmd_len] = c; cmd_len++;
	if (cmd_len > 1) {
		strncpy(custom_status, command, cmd_len);
		update_status();
		c = command[cmd_len];
		command[cmd_len] = '\0';
		num_times = atoi(command);
		command[cmd_len] = c;
		if (num_times < 1) num_times = 1;
	}

	switch (command[cmd_len - 1]) {
	case '#': SCROLL_DOWN(); break;
	case 'd':
		/* TODO: Replace with yank + delete in the movement section */
		read(STDIN_FILENO, &c, 1);
		if (c == '\033') goto end_cmd;
		if (c == 'd') {
			for (i = num_times; i > 0; i--) {
				if (cur_line == line_count) {
					/* Last/only line is a special case */
					if (cur_line > 1) {
						do_cursor_up();
						destroy_line(cur_line_s->next);
						break;
					} else {
						destroy_line(cur_line_s);
						crsr_x = 0; line_shift = 0;
						crsr_restore();
						break;
					}
				} else {
					cur_line_s = cur_line_s->next;
					destroy_line(cur_line_s->prev);
				}
			}
			if (i < num_times) sprintf(custom_status, "Deleted %d lines at %d",
					num_times - i, cur_line);
		}
		break;
	case 'a':	/* append insert */
		/* Append is insert with the cursor moved right */
		vi_mode = MODE_INSERT;
		do_cursor_right();
	case 'i':	/* insert */
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
	case 'o':
		if(alloc_new_line(cur_line, NULL, &line_count, &line_head) == NULL) oom();
		go_to_start_of_next_line();
		redraw_screen(crsr_y, 0);
		vi_mode = MODE_INSERT;
		update_status();
		edit_mode();
		break;
	case 'p':
		sprintf(custom_status, "'put' command not yet supported");
		break;
	case 'x':	/* Delete char at cursor */
		i = num_times;
		for (i = num_times; i > 0; i--) {
			if (do_del_under_crsr(0) == 1) break;
		}
		break;
	case 'X':	/* Delete char left of cursor */
		i = num_times;
		for (i = num_times; i > 0; i--) {
			if (crsr_x == 1) break;
			crsr_x--;
			if (do_del_under_crsr(1) == 1) break;
		}
		break;
#ifndef __ELKS__
	case '!':	/* NON-STANDARD cursor pos dump */
		redraw_screen(0, 0);
		snprintf(custom_status, MAX_STATUS,
				"%dx%d, cx %d, cy %d, ln %d of %d (len %d), clsalsz %d",
				term_cols, term_real_rows, crsr_x, crsr_y,
				cur_line, line_count, cur_line_s->len,
				cur_line_s->alloc_size);
		break;
#endif	/* __ELKS__ */

	case ':':	/* Colon command */
		crsr_yx(term_real_rows, 1);
		ERASE_LINE();
		write(STDOUT_FILENO, ":", 1);
		cmd_len = get_command_string(command);
		if (!cmd_len) break;
		if (strncmp(command, "wq", 2) == 0) {
			/* Save to current file */
			if (cmd_len == 2 && *curfile != '\0') {
				if (save_file(curfile) == 0) goto end_vi;
				else goto end_cmd;
			} else if (cmd_len < 4) {
				strcpy(custom_status, "Cannot save: no file name specified");
				goto end_cmd;
			} else {
				if (save_file(curfile) == 0) goto end_vi;
				else goto end_cmd;
			}
		}
		if (*command == 'w') {
			/* Save the file specified */
			savefile = command + 2;
			i = 0;
			if (cmd_len < 4 || *savefile == '\0') {
				if (*curfile != 0) i = save_file(curfile);
				else sprintf(custom_status, "Cannot save: no file name specified");
			} else i = save_file(savefile);
			if (i) sprintf(custom_status, "Error while saving file");
			goto end_cmd;
		}
		if (strcmp(command, "q") == 0) goto end_vi;
		if (strcmp(command, "q!") == 0) goto end_vi;
		break;

	default:
		sprintf(custom_status, "Unknown key %u", c);
		break;
	}

	/* Any command can jump here to finish up, including status updates */
end_cmd:
	crsr_restore();
	update_status();
	return 0;

end_vi:
	crsr_yx(term_real_rows, 1);
	ERASE_LINE();
	term_restore();
	destroy_buffer(&line_head);
	destroy_buffer(&yank_head);
	exit(EXIT_SUCCESS);
}


int main(int argc, char **argv)
{
	int i;
	char c;
#ifndef NO_SIGNALS
	struct sigaction act;

	/* Set up SIGWINCH handler for window resizing support */
	memset(&act, 0, sizeof(struct sigaction));
	sigemptyset(&act.sa_mask);
	act.sa_sigaction = sigwinch_handler;
	act.sa_flags = SA_SIGINFO;
	sigaction(SIGWINCH, &act, NULL);
#endif	/* NO_SIGNALS */

	/* Start an empty buffer or load a specified file */
	cur_line = 1;
	if (argc == 1) {
		*curfile = '\0';
		cur_line_s = alloc_new_line(line_count, NULL, &line_count, &line_head);
		if (!cur_line_s) {
			fprintf(stderr, "Cannot create initial line\n");
			clean_abort();
		}
	} else {
		strncpy(curfile, argv[1], PATH_MAX);
		i = load_file(curfile, 0);
		if (i == -3) {
			cur_line_s = alloc_new_line(line_count, NULL, &line_count, &line_head);
			if (!cur_line_s) {
				fprintf(stderr, "Cannot create initial line\n");
				clean_abort();
			}
			sprintf(custom_status, "'%s' [NEW FILE]", curfile);
		} else {
			if (i < 0) {
				fprintf(stderr, "Cannot load %s (error %d)\n", curfile, i);
				exit(EXIT_FAILURE);
			}
			cur_line_s = line_head;
			sprintf(custom_status, "Read %d lines from '%s'", i, curfile);
		}
	}

	/* Initialize the terminal */
	if ((i = term_init()) != 0) {
		if (i == -ENOTTY) fprintf(stderr, "a tty is required\n");
		else fprintf(stderr, "cannot init terminal: %s\n", strerror(-i));
		clean_abort();
	}
	read_term_dimensions();
	CLEAR_SCREEN();

	/* Initialize the cursor position and draw the screen */
	crsr_x = 1; crsr_y = 1; line_shift = 0;
	redraw_screen(0, 0);
	update_status();

	/* Read commands forever */
	while (read(STDIN_FILENO, &c, 1)) do_cmd(c);
	clean_abort();
}
