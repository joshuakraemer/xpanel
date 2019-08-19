#include <stdio.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <string.h>
#include <stdlib.h>
#include <uv.h>

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
};

struct x_context {
	xcb_connection_t	*connection;
	int	screen_number;
	xcb_screen_t	*screen;
	xcb_atom_t	atoms[ATOM_COUNT];
};

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
			free(error);
			return FAILURE;
		} else {
			atoms[i] = atom_reply->atom;
			free(atom_reply);
		}
	}

	return SUCCESS;
}

void xcb_callback(uv_poll_t* xcb_poll, int status, int events) {
	if (status < 0) {
		fprintf(stderr, "uv error: %s\n", uv_strerror(status));
		return;
	}

	struct x_context *x_context = (struct x_context *) xcb_poll->data;
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(x_context->connection)) != NULL) {
		if (event->response_type == 0) {
			fprintf(stderr, "Error received: %s\n", xcb_event_get_error_label(XCB_EVENT_RESPONSE_TYPE(event)));
			free(event);
			continue;
		}

		printf("Event received: %s\n", xcb_event_get_label(XCB_EVENT_RESPONSE_TYPE(event)));

		switch (XCB_EVENT_RESPONSE_TYPE(event)) {
			case XCB_EXPOSE:
				break;
			case XCB_CLIENT_MESSAGE:
				if((*(xcb_client_message_event_t *) event).data.data32[0] == x_context->atoms[WM_DELETE_WINDOW])
					exit(EXIT_SUCCESS);
				break;
		}
	}
}

enum status initialize_x(struct x_context *x_context) {
	x_context->connection = xcb_connect(NULL, &x_context->screen_number);
	if (xcb_connection_has_error(x_context->connection) > 0) {
		fputs("X connection has shut down due to a fatal error\n", stderr);
		return FAILURE;
	}
	x_context->screen = xcb_setup_roots_iterator(xcb_get_setup(x_context->connection)).data;

	if (get_atoms(x_context->connection, atom_names, ATOM_COUNT, x_context->atoms) == FAILURE) {
		fputs("Could not get atoms\n", stderr);
		return FAILURE;
	}

	return SUCCESS;
}


int main(int argc, char **argv) {
	int exit_code = EXIT_FAILURE;

	struct x_context x_context;
	if (initialize_x(&x_context) == FAILURE)
		goto out;

	int xcb_socket = xcb_get_file_descriptor(x_context.connection);
	if (xcb_socket == -1) {
		fputs("Could not get XCB socket\n", stderr);
		goto out;
	}

	uv_loop_t *uv_loop = uv_default_loop();
	if (uv_loop == NULL) {
		perror("Could not allocate event loop");
		goto out;
	}

	int uv_status;
	uv_poll_t xcb_poll;
	xcb_poll.data = &x_context;
	if ((uv_status = uv_poll_init(uv_loop, &xcb_poll, xcb_socket)) < 0) {
		fprintf(stderr, "uv_poll_init: %s\n", uv_strerror(uv_status));
		goto out;
	}

	if ((uv_status = uv_poll_start(&xcb_poll, UV_READABLE, &xcb_callback)) < 0) {
		fprintf(stderr, "uv_poll_start: %s\n", uv_strerror(uv_status));
		goto out;
	}

	xcb_window_t window;
	if ((window = xcb_generate_id(x_context.connection)) == -1) {
		fputs("Could not get XID\n", stderr);
		goto out;
	}

	uint32_t mask = XCB_CW_BACK_PIXEL|XCB_CW_EVENT_MASK;
	uint32_t values[] = {
		x_context.screen->white_pixel,
		XCB_EVENT_MASK_EXPOSURE|XCB_EVENT_MASK_BUTTON_PRESS
	};

	xcb_create_window(x_context.connection, XCB_COPY_FROM_PARENT, window, x_context.screen->root, 0, x_context.screen->height_in_pixels - 48, x_context.screen->width_in_pixels, 48, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, x_context.screen->root_visual, mask, values);

	xcb_change_property(x_context.connection, XCB_PROP_MODE_REPLACE, window, x_context.atoms[NET_WM_WINDOW_TYPE], XCB_ATOM_ATOM, 32, 1, &x_context.atoms[NET_WM_WINDOW_TYPE_DOCK]);
	xcb_change_property(x_context.connection, XCB_PROP_MODE_REPLACE, window, x_context.atoms[WM_PROTOCOLS], XCB_ATOM_ATOM, 32, 1, &x_context.atoms[WM_DELETE_WINDOW]);

	xcb_map_window(x_context.connection, window);

	xcb_flush(x_context.connection);


	if ((uv_run(uv_loop, UV_RUN_DEFAULT)) != 0) {
		fputs("Could not run UV loop\n", stderr);
		goto out;
	}

	exit_code = EXIT_SUCCESS;

	out:
		xcb_disconnect(x_context.connection);
		uv_loop_close(uv_loop);
		free(uv_loop);

		return exit_code;
}
