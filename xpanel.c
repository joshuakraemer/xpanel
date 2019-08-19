#include <stdio.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_image.h>

#include <string.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <time.h>
#include <locale.h>
#include <iconv.h>
#include <langinfo.h> // nl_langinfo()
#include <x86intrin.h> // __rdtsc()
#include <fontconfig/fontconfig.h>
//#include <ft2build.h>
//#include FT_FREETYPE_H
//#include <pango/pangoft2.h>
#include "xcbft/xcbft.h"


#include <unistd.h> // pause()


#define WINDOW_LIST_SIZE 256
#define WINDOW_TITLE_SIZE 128

/* used as return values for functions */
enum status {
	SUCCESS,
	FAILURE
};

/* used as index, keep synchronized with atom_names */
enum {
	WM_DELETE_WINDOW,
	WM_PROTOCOLS,
	NET_ACTIVE_WINDOW,
	NET_CLIENT_LIST,
	NET_CURRENT_DESKTOP,
	NET_WM_NAME,
	NET_WM_WINDOW_TYPE,
	NET_WM_WINDOW_TYPE_DOCK,
	UTF8_STRING,
	ATOM_COUNT
};

static char *atom_names[] = {
	"WM_DELETE_WINDOW",
	"WM_PROTOCOLS",
	"_NET_ACTIVE_WINDOW",
	"_NET_CLIENT_LIST",
	"_NET_CURRENT_DESKTOP",
	"_NET_WM_NAME",
	"_NET_WM_WINDOW_TYPE",
	"_NET_WM_WINDOW_TYPE_DOCK",
	"UTF8_STRING"
};

struct x11 {
	xcb_connection_t	*connection;
	int	screen_number;
	xcb_screen_t	*screen;
	xcb_atom_t	atoms[ATOM_COUNT];
};

struct iconv {
	iconv_t latin1;
	iconv_t utf8;
};

enum status transcode(iconv_t converter, char *input_buffer, size_t input_size, char *output_buffer, size_t output_size) {
	char *input_position = input_buffer;
	size_t input_remaining = input_size;
	char *output_position = output_buffer;
	size_t output_remaining = output_size;

	iconv(converter, NULL, NULL, NULL, NULL);
	if (iconv(converter, &input_position, &input_remaining, &output_position, &output_remaining) == (size_t) -1) {
		perror("iconv");
		//return FAILURE;
		// TODO: error handling
	}
	output_buffer[output_size - output_remaining] = '\0';

	return SUCCESS;
}

xcb_screen_t *get_screen(xcb_connection_t *connection, int screen_number) {
	for (xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(xcb_get_setup(connection));
			iterator.rem;
			--screen_number, xcb_screen_next(&iterator)) {
		if (screen_number == 0)
			return iterator.data;
	}
	return NULL;
}

enum status get_atoms(xcb_connection_t *connection, char **atom_names, int atom_count, xcb_atom_t *atoms) {
	xcb_intern_atom_cookie_t atom_cookies[atom_count];
	for (int i = 0; i < atom_count; ++i)
		atom_cookies[i] = xcb_intern_atom(connection, 0, strlen(atom_names[i]), atom_names[i]);

	xcb_intern_atom_reply_t *atom_reply;
	xcb_generic_error_t *error;

	for (int i = 0; i < atom_count; ++i) {
		if ((atom_reply = xcb_intern_atom_reply(connection, atom_cookies[i], &error)) == NULL) {
			fprintf(stderr, "X error: %d\n", error->error_code);
			free(error);
			return FAILURE;
		} else {
			atoms[i] = atom_reply->atom;
			free(atom_reply);
		}
	}

	return SUCCESS;
}

enum status initialize_x(struct x11 *x11) {
	x11->connection = xcb_connect(NULL, &x11->screen_number);
	if (xcb_connection_has_error(x11->connection) > 0) {
		fputs("X connection has shut down due to a fatal error\n", stderr);
		return FAILURE;
	}
	x11->screen = xcb_setup_roots_iterator(xcb_get_setup(x11->connection)).data;

	if (get_atoms(x11->connection, atom_names, ATOM_COUNT, x11->atoms) == FAILURE) {
		fputs("Could not get atoms\n", stderr);
		return FAILURE;
	}

	return SUCCESS;
}

enum status get_property_reply(xcb_connection_t *connection, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, uint8_t format, size_t size, xcb_get_property_reply_t **reply_return) {
	/* long_length: number of 4-byte units to be retrieved, divide by 4 and round up to get at least 'size' bytes */
	uint32_t long_length = size/4 + (size%4 > 0);
	xcb_get_property_cookie_t cookie = xcb_get_property(connection, 0, window, property, type, 0, long_length);

	xcb_get_property_reply_t *reply = NULL;
	xcb_generic_error_t *error = NULL;
	if ((reply = xcb_get_property_reply(connection, cookie, &error)) == NULL) {
		fprintf(stderr, "X error: %d\n", error->error_code);
		free(error);
		return FAILURE;
	}

	if (reply->type != type || reply->format != format) {
		free(reply);
		return FAILURE;
	}

	*reply_return = reply;
	return SUCCESS;
}

enum status get_client_list(struct x11 *x11, xcb_window_t *window_buffer, size_t buffer_size, int *window_count) {
	xcb_get_property_reply_t *reply = NULL;
	if (get_property_reply(x11->connection, x11->screen->root, x11->atoms[NET_CLIENT_LIST], XCB_ATOM_WINDOW, 32, buffer_size, &reply) == FAILURE)
		return FAILURE;

	memcpy(window_buffer, xcb_get_property_value(reply), xcb_get_property_value_length(reply));
	*window_count = xcb_get_property_value_length(reply)/sizeof (xcb_window_t);
	free(reply);
	return SUCCESS;
}

enum status get_title(struct x11 *x11, struct iconv *iconv, xcb_window_t window, char *string_buffer, size_t buffer_size) {
	enum status return_code = FAILURE;
	xcb_get_property_reply_t *reply = NULL;

	/* (buffer_size - 1)*4: in UTF-8, one character may have up to 4 bytes */
	if (get_property_reply(x11->connection, window, x11->atoms[NET_WM_NAME], x11->atoms[UTF8_STRING], 8, (buffer_size - 1)*4, &reply) == SUCCESS)
		return_code = transcode(iconv->utf8, xcb_get_property_value(reply), xcb_get_property_value_length(reply), string_buffer, buffer_size);
	else
		if (get_property_reply(x11->connection, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, buffer_size - 1, &reply) == SUCCESS)
			return_code = transcode(iconv->latin1, xcb_get_property_value(reply), xcb_get_property_value_length(reply), string_buffer, buffer_size);

	free(reply);
	return return_code;
}

enum status get_class(struct x11 *x11, xcb_window_t window, char *string_buffer, size_t buffer_size) {
	xcb_get_property_reply_t *reply = NULL;
	if (get_property_reply(x11->connection, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, buffer_size, &reply) == FAILURE)
		return FAILURE;

	/* get the first \0-terminated string in the property, i.e. the instance name */
	char *string = xcb_get_property_value(reply);
	size_t string_size = strnlen(string, xcb_get_property_value_length(reply));
	size_t copy_size = string_size < buffer_size - 1 ? string_size : buffer_size - 1;
	memcpy(string_buffer, string, copy_size);
	string_buffer[copy_size] = '\0';

	return SUCCESS;
}

void handle_event(struct x11 *x11, xcb_generic_event_t *event) {
	if (event->response_type == 0) {
		puts("event->response_type == 0");
		fprintf(stderr, "X error: %s\n", xcb_event_get_error_label(((xcb_generic_error_t *)event)->error_code));
		free(event);
		return;
	}

	printf("Event received: %s\n", xcb_event_get_label(XCB_EVENT_RESPONSE_TYPE(event)));

	switch (XCB_EVENT_RESPONSE_TYPE(event)) {
		case XCB_EXPOSE:
			break;
		case XCB_CLIENT_MESSAGE:
			if((*(xcb_client_message_event_t *) event).data.data32[0] == x11->atoms[WM_DELETE_WINDOW])
				exit(EXIT_SUCCESS);
			break;
	}
	return;
}

enum status initialize_iconv(struct iconv *iconv) {
	//char *locale_charset = locale_charset();
	char *locale_charset = nl_langinfo(CODESET);
	size_t length = strlen(locale_charset);
	char target_charset[length + 11];
	memcpy(target_charset, locale_charset, length);
	memcpy(target_charset + length, "//TRANSLIT", 11);

	if ((iconv->latin1 = iconv_open(target_charset, "ISO-8859-1")) == (iconv_t) -1) {
		perror("iconv_open");
		return FAILURE;
	}
	if ((iconv->utf8 = iconv_open(target_charset, "UTF-8")) == (iconv_t) -1) {
		perror("iconv_open");
		return FAILURE;
	}

	return SUCCESS;
}


int main(int argc, char **argv) {
	int exit_code = EXIT_FAILURE;
	setlocale(LC_ALL, "");

	struct x11 x11;
	if (initialize_x(&x11) == FAILURE)
		goto out;

	struct iconv iconv;
	if (initialize_iconv(&iconv) == FAILURE)
		goto out;

	xcb_window_t window;
	if ((window = xcb_generate_id(x11.connection)) == -1) {
		fputs("Could not get XID\n", stderr);
		goto out;
	}

	uint32_t mask = XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK;
	uint32_t values[] = {
		x11.screen->white_pixel,
		XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_BUTTON_PRESS
	};

	xcb_create_window(x11.connection, XCB_COPY_FROM_PARENT, window, x11.screen->root, 0, x11.screen->height_in_pixels - 48, x11.screen->width_in_pixels, 48, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, x11.screen->root_visual, mask, values);

	xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, window, x11.atoms[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &x11.atoms[NET_WM_WINDOW_TYPE_DOCK]);
	xcb_change_property(x11.connection, XCB_PROP_MODE_REPLACE, window, x11.atoms[WM_PROTOCOLS], XCB_ATOM_ATOM, 32, 1, &x11.atoms[WM_DELETE_WINDOW]);

	xcb_map_window(x11.connection, window);

	xcb_flush(x11.connection);


	xcb_window_t windows[WINDOW_LIST_SIZE];
	int window_count;
	if (get_client_list(&x11, windows, sizeof windows, &window_count) == FAILURE) {
		fputs("Could not get client list\n", stderr);
		goto out;
	}

	char window_title[WINDOW_TITLE_SIZE];
	char window_class[WINDOW_TITLE_SIZE];

	for (int i = 0; i < window_count; ++i) {
		printf("(0x%x) ", windows[i]);
		get_class(&x11, windows[i], window_class, WINDOW_TITLE_SIZE);
		printf("%s: ", window_class);

		if (get_title(&x11, &iconv, windows[i], window_title, sizeof window_title) == FAILURE) {
			puts("<unknown>");
		} else {
			printf("%s\n", window_title);
		}


	}


	/*
	char buf[WINDOW_TITLE_SIZE];
	unsigned long long start, diff;

	start = __rdtsc();
	for (long i = 0; i < 1000000; ++i) {

		get_title(&x11, &iconv, 0x1200001, buf, WINDOW_TITLE_SIZE);
		//printf("buf: '%s'\n", buf);

	}
	diff = __rdtsc() - start;
	printf("diff: %'lld\n", diff);

	start = __rdtsc();
	for (long i = 0; i < 1000000; ++i) {

		get_title(&x11, &iconv, 0x1200001, buf, WINDOW_TITLE_SIZE);
		//printf("buf: '%s'\n", buf);

	}
	diff = __rdtsc() - start;
	printf("diff: %'lld\n", diff);

	exit(0);
	*/












FcStrSet *fontsearch;
struct xcbft_patterns_holder font_patterns;
struct utf_holder text;
struct xcbft_face_holder faces;
xcb_render_color_t text_color;

// The pixmap we want to draw over
//xcb_pixmap_t pmap = xcb_generate_id(c);

/* ... pmap stuffs fill and others ... */

// The fonts to use and the text in unicode
char *searchlist = "Times New Roman:style=bold:pixelsize=24\n";
text = char_to_uint32("Héllo ༃𐤋𐤊탄ཀ𐍊");

// extract the fonts in a list
fontsearch = xcbft_extract_fontsearch_list(searchlist);
// do the search and it returns all the matching fonts
font_patterns = xcbft_query_fontsearch_all(fontsearch);
// no need for the fonts list anymore
FcStrSetDestroy(fontsearch);
// get the dpi from the resources or the screen if not available
long dpi = xcbft_get_dpi(x11.connection);
// load the faces related to the matching fonts patterns
faces = xcbft_load_faces(font_patterns, dpi);
// no need for the matching fonts patterns
xcbft_patterns_holder_destroy(font_patterns);

// select a specific color
text_color.red =  0x4242;
text_color.green = 0x4242;
text_color.blue = 0x4242;
text_color.alpha = 0xFFFF;

// draw on the drawable (pixmap here) pmap at position (50,60) the text
// with the color we chose and the faces we chose
xcbft_draw_text(
	x11.connection, // X connection
	window, // win or pixmap
	0, 30, // x, y
	text, // text
	text_color,
	faces,
	dpi);

// no need for the text and the faces
utf_holder_destroy(text);
xcbft_face_holder_destroy(faces);


xcb_flush(x11.connection);





	xcb_generic_event_t *event = NULL;
	while ((event = xcb_poll_for_queued_event(x11.connection)) != NULL)
		handle_event(&x11, event);

	puts("All queued events processd\n");

	if (xcb_connection_has_error(x11.connection) > 0) {
		fputs("X connection has shut down due to a fatal error\n", stderr);
		goto out;
	}

	struct pollfd pollfd = {
		.fd = xcb_get_file_descriptor(x11.connection),
		.events = POLLIN
	};

	while (1) {
		struct timespec current_timespec = {0};
		if (clock_gettime(CLOCK_REALTIME, &current_timespec) == -1) {
			perror("clock_gettime");
			goto out;
		}
		time_t current_time = current_timespec.tv_sec;

		/* timestamp of next full minute */
		time_t next_time = current_time - (current_time % 60) + 60;

		/* time until next full minute in ms */
		int time_span = (next_time - current_time)*1000 - current_timespec.tv_nsec/1000000;

		int poll_result = poll(&pollfd, 1, time_span);
		switch (poll_result) {
			case -1: /* error */
				puts("Error");
				break;
			case 0:; /* timeout */
				struct tm *next_tm = localtime(&next_time);
				printf("Next time = %02d:%02d:%02d\n", next_tm->tm_hour, next_tm->tm_min, next_tm->tm_sec);
				break;
			default: /* XCB socket is ready */
				if (pollfd.revents != POLLIN) {
					puts("Unexpected poll event");
					goto out;
				}
				while ((event = xcb_poll_for_event(x11.connection)) != NULL)
					handle_event(&x11, event);
		}
	}

	exit_code = EXIT_SUCCESS;

	out:
		xcb_disconnect(x11.connection);

		return exit_code;
}
